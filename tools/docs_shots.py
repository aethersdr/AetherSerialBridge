#!/usr/bin/env python3
"""Renders the documentation images from the demo fixture (INV-135, v1.1 WS-19).

    docs_shots.py --os windows|linux|pi --bin <test build's bin dir> --out <dir> [--fixture docs/images/demo]

What it does, in order:
  1. Refuses any fixture that is not this repository's docs/images/demo (symlinks resolved first), so no image can
     be made from a real configuration.
  2. Isolates the settings: HOME, USERPROFILE, XDG_*, APPDATA and LOCALAPPDATA point into a new temporary folder, so
     nothing the test binaries could read (AetherSDR's theme settings included) is the runner's.
  3. Runs the test build's hooks: `tst_BridgeWindow docsShots` (windows, linux) at scale 1 and again at scale 2 for
     the hero image, and `tst_Tui docsShots` (every OS). Each writes PNGs (the window) or SGR frames (.ans, the TUI)
     and beside each a manifest <name>.json of every text it shows.
  4. Renders each frame to PNG with tools/ansi_to_png.py, optimises every PNG (tools/ansi_to_png.py --optimize:
     Pillow, palette where lossless, no metadata), and refuses an image without its manifest or a manifest with a
     finding of tools/check_docs_images.py.

The output folder then holds <name>.png + <name>.json pairs only; the WS-19 builder commits them under
docs/images/<os>/ after looking at each one. Exit 0 on success, 2 when refused, 1 when a step fails.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEMO = ROOT / "docs" / "images" / "demo"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_docs_images as gate  # noqa: E402


class Refused(Exception):
    pass


def demo_fixture(path):
    """The fixture folder, only when it is this repository's docs/images/demo (INV-135)."""
    p = Path(path)
    if not p.is_absolute():
        p = Path.cwd() / p
    real = p.resolve(strict=False)
    if not real.is_dir():
        raise Refused(f"refused: fixture {path} is not a folder")
    if real != DEMO.resolve():
        raise Refused(f"refused: fixture {path} is not the demo fixture docs/images/demo (INV-135)")
    return real


def isolated_env(home, raw, fixture, os_name, base=None):
    env = dict(os.environ if base is None else base)
    h = Path(home)
    for k, sub in (("HOME", ""), ("USERPROFILE", ""), ("XDG_CONFIG_HOME", ".config"), ("XDG_DATA_HOME", ".local/share"),
                   ("XDG_CACHE_HOME", ".cache"), ("XDG_STATE_HOME", ".local/state"), ("XDG_RUNTIME_DIR", "run"),
                   ("APPDATA", "AppData/Roaming"), ("LOCALAPPDATA", "AppData/Local")):
        d = h / sub if sub else h
        d.mkdir(parents=True, exist_ok=True)
        if k == "XDG_RUNTIME_DIR":
            d.chmod(0o700)
        env[k] = str(d)
    for k in ("XDG_CONFIG_DIRS", "XDG_DATA_DIRS", "QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_FONT_DPI",
              "ASB_SCREENSHOT_DIR", "ASB_DOCS_SHOTS_HERO"):
        env.pop(k, None)
    env.update({
        # Windows renders on its native platform: offscreen there has no system font fallback, and Inter lacks the
        # close button's and the Details button's symbols. Linux and the Pi get fontconfig's fallback offscreen.
        "QT_QPA_PLATFORM": "windows" if os_name == "windows" else "offscreen",
        "TZ": "UTC",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "ASB_DOCS_SHOTS_DIR": str(raw),
        "ASB_DOCS_SHOTS_HOME": str(h),
        "ASB_DOCS_FIXTURE": str(fixture),
        "ASB_DOCS_SHOTS_OS": os_name,
    })
    return env


def exe(bindir, name):
    for n in (name, name + ".exe"):
        p = Path(bindir) / n
        if p.is_file():
            return p
    raise FileNotFoundError(f"{name} not found in {bindir}")


def run(cmd, env):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], env=env)
    if r.returncode != 0:
        raise RuntimeError(f"{cmd[0]} exited {r.returncode}")


def generate(os_name, bindir, out, fixture, converter, optimize=True, base_env=None):
    fixture = demo_fixture(fixture)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        raise Refused(f"refused: {out} is not empty")
    with tempfile.TemporaryDirectory(prefix="asb-docs-home-") as home, \
            tempfile.TemporaryDirectory(prefix="asb-docs-raw-") as raw:
        env = isolated_env(home, raw, fixture, os_name, base_env)
        if os_name in ("windows", "linux"):
            window = exe(bindir, "tst_BridgeWindow")
            run([window, "docsShots"], dict(env, QT_SCALE_FACTOR="1"))
            run([window, "docsShots"], dict(env, QT_SCALE_FACTOR="2", ASB_DOCS_SHOTS_HERO="1"))
        run([exe(bindir, "tst_Tui"), "docsShots"], env)

        raw = Path(raw)
        frames = sorted(raw.glob("*.ans"))
        for f in frames:
            run([sys.executable, converter, f, f.with_suffix(".png")], env)
            f.unlink()
        pngs = sorted(raw.glob("*.png"))
        if not pngs:
            raise RuntimeError("the hooks wrote no image")
        if optimize:
            run([sys.executable, converter, "--optimize", *pngs], env)
        allow = gate.load_allow(ROOT)
        findings = []
        for p in pngs:
            m = p.with_suffix(".json")
            if not m.exists():
                findings.append(f"{p.name}: no manifest")
                continue
            data = json.loads(m.read_text(encoding="utf-8"))
            if data.get("image") != p.name or not data.get("texts"):
                findings.append(f"{m.name}: does not describe {p.name}")
            findings += [f"{m.name}: {x}" for x in gate.manifest_findings(data, allow)]
        stray = [p.name for p in raw.iterdir() if p.suffix not in (".png", ".json")]
        stray += [m.name for m in raw.glob("*.json") if not m.with_suffix(".png").exists()]
        if stray:
            findings.append(f"unexpected files: {sorted(stray)}")
        if findings:
            raise RuntimeError("manifest check failed:\n  " + "\n  ".join(findings))
        for p in sorted(raw.iterdir()):
            (out / p.name).write_bytes(p.read_bytes())
    print(f"docs_shots: {len(pngs)} images in {out}")
    return pngs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--os", required=True, choices=("windows", "linux", "pi"))
    ap.add_argument("--bin", required=True, help="the test build's bin folder (tst_BridgeWindow, tst_Tui)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--fixture", default=str(DEMO))
    ap.add_argument("--converter", default=str(ROOT / "tools" / "ansi_to_png.py"))
    ap.add_argument("--no-optimize", action="store_true")
    a = ap.parse_args(argv)
    try:
        generate(a.os, a.bin, a.out, a.fixture, a.converter, optimize=not a.no_optimize)
    except Refused as e:
        print(f"docs_shots: {e}", file=sys.stderr)
        return 2
    except (RuntimeError, FileNotFoundError) as e:
        print(f"docs_shots: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
