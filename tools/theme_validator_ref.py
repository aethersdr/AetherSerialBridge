#!/usr/bin/env python3
"""Reference theme validator: the test oracle for src/gui/ThemeValidator (WS-11, INV-36).

This is an independent implementation of the validator rules, which mirror AetherSDR's own value
discrimination in ThemeManager.cpp `flattenTokens` (an object with a string `type` is a
gradient, one with a string `family` is a font, any other object is a group). The Qt validator
must agree with it on every fixture in tests/data/themes/manifest.json.

Usage:
  theme_validator_ref.py FILE...                 print PASS/FAIL, counts or the failing path
  theme_validator_ref.py --write-manifest DIR    (re)write DIR/manifest.json from the fixtures
  theme_validator_ref.py --check-manifest DIR    exit 1 unless every fixture still gives the
                                                 recorded result (the CI agreement check)
  theme_validator_ref.py --color-names           the colour words accepted, one per line

Result fields: result (PASS/FAIL); on PASS leaves, gradients, fonts; on FAIL code and path.
`leaves` counts scalar leaves outside gradient and font objects (the measure the spec's evidence
uses: Default Dark 223, Default Light 224); gradients and fonts are counted separately.

Rules (each has a failure code; the first failure in document order, keys visited in sorted
order, is reported):
  size       more than 256 KiB
  document   not UTF-8 JSON, or not an object (a leading UTF-8 BOM is accepted, as Qt does;
             NaN, Infinity and numbers that overflow a double are parse errors, as in Qt)
  schema     schemaVersion missing, not a number, or not exactly 1 or 2
  base-theme baseTheme present but not "Default Dark" or "Default Light"
  section    tokens / primitives / scopes present but not an object
  top-key    any other top-level key (name, author, version, description are skipped)
  depth      a group nested deeper than 12 (the section object is depth 1)
  key        a group key not matching ^[A-Za-z0-9._/-]{1,96}$
  array      an array where a value is expected
  null       a null value
  number     a number that is not finite or has |x| > 10000
  string     a string that is not an alias {..}, a hex colour, a lowercase colour word, or
             (only below a "font" key) a font family
  leaves     more than 5000 scalar leaves
  gradient-key / gradient-type / gradient-number / gradient-center / gradient-stops /
  stop-key / stop-at / stop-color      the gradient rules
  font-key / font-family / font-size / font-color                      the font rules
"""

import json
import math
import os
import re
import sys

MAX_BYTES = 256 * 1024
MAX_DEPTH = 12
MAX_LEAVES = 5000
MAX_STOPS = 32
MAX_ABS_NUMBER = 10000.0

METADATA = {"name", "author", "version", "description", "schemaVersion"}
SECTIONS = {"tokens", "primitives", "scopes"}
BASE_THEMES = {"Default Dark", "Default Light"}
GRADIENT_KEYS = {"type", "angle", "stops", "center", "radius"}
GRADIENT_TYPES = {"linear-gradient", "radial-gradient"}
STOP_KEYS = {"at", "color"}
FONT_KEYS = {"family", "size", "color"}

KEY_RE = re.compile(r"[A-Za-z0-9._/-]{1,96}")
ALIAS_RE = re.compile(r"\{[A-Za-z0-9._-]{1,96}\}")
HEX_RE = re.compile(r"#(?:[0-9A-Fa-f]{3}|[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})")
WORD_RE = re.compile(r"[a-z]+")
FAMILY_RE = re.compile(r"[A-Za-z0-9 _-]{1,64}")

# The colour names QColor::isValidColorName accepts (Qt 6: the SVG 1.0 keywords + transparent).
# tst_ThemeFollower::colorNamesMatchOracle compares this list with QColor::colorNames().
COLOR_NAMES = frozenset("""
aliceblue antiquewhite aqua aquamarine azure beige bisque black blanchedalmond blue blueviolet
brown burlywood cadetblue chartreuse chocolate coral cornflowerblue cornsilk crimson cyan darkblue
darkcyan darkgoldenrod darkgray darkgreen darkgrey darkkhaki darkmagenta darkolivegreen darkorange
darkorchid darkred darksalmon darkseagreen darkslateblue darkslategray darkslategrey darkturquoise
darkviolet deeppink deepskyblue dimgray dimgrey dodgerblue firebrick floralwhite forestgreen
fuchsia gainsboro ghostwhite gold goldenrod gray green greenyellow grey honeydew hotpink indianred
indigo ivory khaki lavender lavenderblush lawngreen lemonchiffon lightblue lightcoral lightcyan
lightgoldenrodyellow lightgray lightgreen lightgrey lightpink lightsalmon lightseagreen
lightskyblue lightslategray lightslategrey lightsteelblue lightyellow lime limegreen linen magenta
maroon mediumaquamarine mediumblue mediumorchid mediumpurple mediumseagreen mediumslateblue
mediumspringgreen mediumturquoise mediumvioletred midnightblue mintcream mistyrose moccasin
navajowhite navy oldlace olive olivedrab orange orangered orchid palegoldenrod palegreen
paleturquoise palevioletred papayawhip peachpuff peru pink plum powderblue purple red rosybrown
royalblue saddlebrown salmon sandybrown seagreen seashell sienna silver skyblue slateblue
slategray slategrey snow springgreen steelblue tan teal thistle tomato transparent turquoise
violet wheat white whitesmoke yellow yellowgreen
""".split())


class Fail(Exception):
    def __init__(self, code, path):
        super().__init__(f"{code} at {path}")
        self.code = code
        self.path = path


def join(path, key):
    return key if not path else f"{path}.{key}"


def is_number(v):
    # bool is an int in Python; JSON true/false are never numbers here (Qt: isDouble() is false).
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def finite_bounded(v):
    return is_number(v) and math.isfinite(float(v)) and abs(float(v)) <= MAX_ABS_NUMBER


def is_color(s):
    return bool(ALIAS_RE.fullmatch(s) or HEX_RE.fullmatch(s)
                or (WORD_RE.fullmatch(s) and s in COLOR_NAMES))


class Validator:
    def __init__(self):
        self.leaves = 0
        self.gradients = 0
        self.fonts = 0

    def run(self, data):
        if len(data) > MAX_BYTES:
            raise Fail("size", "")
        if data.startswith(b"\xef\xbb\xbf"):
            data = data[3:]

        def no_constant(_name):
            raise ValueError("non-finite literal")

        def strict_float(text):
            v = float(text)
            if not math.isfinite(v):
                raise ValueError("number overflows a double")
            return v

        try:
            doc = json.loads(data.decode("utf-8", errors="strict"),
                             parse_constant=no_constant, parse_float=strict_float)
        except (UnicodeDecodeError, ValueError):
            raise Fail("document", "")
        if not isinstance(doc, dict):
            raise Fail("document", "")

        sv = doc.get("schemaVersion")
        if not is_number(sv) or float(sv) not in (1.0, 2.0):
            raise Fail("schema", "schemaVersion")

        for key in sorted(doc):
            value = doc[key]
            if key in METADATA:
                continue
            if key == "baseTheme":
                if not (isinstance(value, str) and value in BASE_THEMES):
                    raise Fail("base-theme", key)
            elif key in SECTIONS:
                if not isinstance(value, dict):
                    raise Fail("section", key)
                self.group(value, key, [key], 1)
            else:
                raise Fail("top-key", key)

    # segments: every key from the section down, each split on '.', as the loader flattens them.
    def group(self, obj, path, segments, depth):
        if depth > MAX_DEPTH:
            raise Fail("depth", path)
        for key in sorted(obj):
            value = obj[key]
            child = join(path, key)
            if not KEY_RE.fullmatch(key):
                raise Fail("key", child)
            child_segments = segments + key.split(".")
            if isinstance(value, dict):
                if isinstance(value.get("type"), str):
                    self.gradient(value, child)
                elif isinstance(value.get("family"), str):
                    self.font(value, child)
                else:
                    self.group(value, child, child_segments, depth + 1)
                continue
            if isinstance(value, list):
                raise Fail("array", child)
            if value is None:
                raise Fail("null", child)
            if isinstance(value, bool):
                pass
            elif is_number(value):
                if not finite_bounded(value):
                    raise Fail("number", child)
            else:  # str
                below_font = "font" in child_segments[:-1]
                if not (is_color(value) or (below_font and FAMILY_RE.fullmatch(value))):
                    raise Fail("string", child)
            self.leaves += 1
            if self.leaves > MAX_LEAVES:
                raise Fail("leaves", child)

    def gradient(self, obj, path):
        for key in sorted(obj):
            if key not in GRADIENT_KEYS:
                raise Fail("gradient-key", join(path, key))
        for key in sorted(obj):
            value = obj[key]
            child = join(path, key)
            if key == "type":
                if value not in GRADIENT_TYPES:
                    raise Fail("gradient-type", child)
            elif key in ("angle", "radius"):
                if not finite_bounded(value):
                    raise Fail("gradient-number", child)
            elif key == "center":
                if not (isinstance(value, list) and len(value) == 2
                        and all(finite_bounded(c) for c in value)):
                    raise Fail("gradient-center", child)
            elif key == "stops":
                if not isinstance(value, list) or len(value) > MAX_STOPS:
                    raise Fail("gradient-stops", child)
                for i, stop in enumerate(value):
                    self.stop(stop, f"{child}[{i}]")
        self.gradients += 1

    def stop(self, stop, path):
        if not isinstance(stop, dict):
            raise Fail("gradient-stops", path)
        for key in sorted(stop):
            if key not in STOP_KEYS:
                raise Fail("stop-key", join(path, key))
        for key in sorted(stop):
            value = stop[key]
            child = join(path, key)
            if key == "at":
                if not (is_number(value) and 0.0 <= float(value) <= 1.0):
                    raise Fail("stop-at", child)
            elif not (isinstance(value, str) and is_color(value)):
                raise Fail("stop-color", child)

    def font(self, obj, path):
        for key in sorted(obj):
            if key not in FONT_KEYS:
                raise Fail("font-key", join(path, key))
        for key in sorted(obj):
            value = obj[key]
            child = join(path, key)
            if key == "family":
                if not FAMILY_RE.fullmatch(value):
                    raise Fail("font-family", child)
            elif key == "size":
                if not (is_number(value) and 0.0 <= float(value) <= 200.0):
                    raise Fail("font-size", child)
            elif not (isinstance(value, str) and is_color(value)):
                raise Fail("font-color", child)
        self.fonts += 1


def validate(data):
    v = Validator()
    try:
        v.run(data)
    except Fail as f:
        return {"result": "FAIL", "code": f.code, "path": f.path}
    return {"result": "PASS", "leaves": v.leaves, "gradients": v.gradients, "fonts": v.fonts}


# --- manifest ----------------------------------------------------------------------------------
# Fixture files are every *.json in the fixture directory except manifest.json, plus the two
# vendored built-ins (paths relative to the repository root, which is two levels above DIR's
# parent: tests/data/themes -> repo).
BUILT_INS = ["vendor/aethersdr/resources/themes/default-dark.json",
             "vendor/aethersdr/resources/themes/default-light.json"]


def fixture_entries(directory):
    repo = os.path.normpath(os.path.join(directory, "..", "..", ".."))
    entries = [(b, os.path.join(repo, b)) for b in BUILT_INS]
    for name in sorted(os.listdir(directory)):
        if name.endswith(".json") and name != "manifest.json":
            entries.append((f"tests/data/themes/{name}", os.path.join(directory, name)))
    return entries


def compute_manifest(directory):
    out = []
    for rel, full in fixture_entries(directory):
        with open(full, "rb") as f:
            out.append({"file": rel, **validate(f.read())})
    return {"comment": "Generated by tools/theme_validator_ref.py --write-manifest; the Qt "
                       "validator must give the same result for every file (tst_ThemeFollower).",
            "fixtures": out}


def main(argv):
    if len(argv) == 2 and argv[1] == "--color-names":
        print("\n".join(sorted(COLOR_NAMES)))
        return 0
    if len(argv) == 3 and argv[1] in ("--write-manifest", "--check-manifest"):
        directory = argv[2]
        manifest_path = os.path.join(directory, "manifest.json")
        computed = compute_manifest(directory)
        if argv[1] == "--write-manifest":
            with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(computed, f, indent=2, sort_keys=True)
                f.write("\n")
            print(f"wrote {manifest_path}: {len(computed['fixtures'])} fixtures")
            return 0
        with open(manifest_path, encoding="utf-8") as f:
            recorded = json.load(f)
        if recorded.get("fixtures") != computed["fixtures"]:
            rec = {e["file"]: e for e in recorded.get("fixtures", [])}
            com = {e["file"]: e for e in computed["fixtures"]}
            for name in sorted(set(rec) | set(com)):
                if rec.get(name) != com.get(name):
                    print(f"MISMATCH {name}: manifest {rec.get(name)} oracle {com.get(name)}")
            return 1
        print(f"oracle agrees with {manifest_path}: {len(computed['fixtures'])} fixtures")
        return 0
    if len(argv) < 2 or argv[1].startswith("-"):
        print(__doc__, file=sys.stderr)
        return 2
    rc = 0
    for path in argv[1:]:
        with open(path, "rb") as f:
            r = validate(f.read())
        if r["result"] == "PASS":
            print(f"PASS {path}: {r['leaves']} leaves, {r['gradients']} gradients, "
                  f"{r['fonts']} fonts")
        else:
            print(f"FAIL {path}: {r['code']} at {r['path'] or '<document>'}")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
