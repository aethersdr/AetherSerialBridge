#!/usr/bin/env python3
"""Renders one TUI frame (FTXUI's output: text with SGR codes) to a PNG, for the documentation images (v1.1 WS-19).

    ansi_to_png.py <frame.ans> <out.png>
    ansi_to_png.py --optimize <png> [<png> ...]     re-saves each PNG optimised, as a palette image where that is
                                                    lossless, with no metadata

The frame is drawn cell by cell in DejaVu Sans Mono 2.37 (downloaded once into the font cache, $ASB_DOCS_FONT_CACHE
or the system temporary folder, and used only when its SHA-256 matches the pins below; nothing is committed), on the
aethersdr.com palette the TUI uses (src/tui/TuiPalette.h: the terminal's own default colours are the palette's ink and
background). Block elements (the logo's half blocks) are filled as rectangles, so cells meet without seams.

Needs Pillow PILLOW_VERSION exactly (the same pin as tools/gen_logo_art.py).
"""
import hashlib
import io
import os
import re
import sys
import tarfile
import tempfile
import unicodedata
import urllib.request
from pathlib import Path

PILLOW_VERSION = "11.0.0"
FONT_URL = "https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2"
FONT_ARCHIVE_SHA256 = "fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7"
FONTS = {  # member of the archive -> SHA-256
    "regular": ("dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono.ttf",
                "b4a6c3e4faab8773f4ff761d56451646409f29abedd68f05d38c2df667d3c582"),
    "bold": ("dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono-Bold.ttf",
             "bce60f1b4421acd9ea51ba6623d7024ecbe6817a953e3654df62a5e6bdf8f769"),
}
FONT_PX = 16
PAD = 12
INK = (0xEA, 0xF2, 0xFB)   # palette::kInk
BG = (0x06, 0x0B, 0x13)    # palette::kBg
# xterm's 16 colours (FTXUI's Basic16 names), for a frame that uses them.
BASIC16 = [(0, 0, 0), (205, 0, 0), (0, 205, 0), (205, 205, 0), (0, 0, 238), (205, 0, 205), (0, 205, 205),
           (229, 229, 229), (127, 127, 127), (255, 0, 0), (0, 255, 0), (255, 255, 0), (92, 92, 255), (255, 0, 255),
           (0, 255, 255), (255, 255, 255)]
CSI = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")


def need_pillow():
    try:
        import PIL
        from PIL import Image, ImageDraw, ImageFont  # noqa: F401
    except ImportError:
        sys.exit(f"ansi_to_png.py: needs Pillow {PILLOW_VERSION} (pip install Pillow=={PILLOW_VERSION})")
    if PIL.__version__ != PILLOW_VERSION:
        sys.exit(f"ansi_to_png.py: needs Pillow {PILLOW_VERSION} exactly (found {PIL.__version__})")


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def font_files():
    cache = Path(os.environ.get("ASB_DOCS_FONT_CACHE") or Path(tempfile.gettempdir()) / "asb-docs-fonts")
    cache.mkdir(parents=True, exist_ok=True)
    out = {k: cache / Path(member).name for k, (member, _) in FONTS.items()}
    if all(p.exists() and sha256(p.read_bytes()) == FONTS[k][1] for k, p in out.items()):
        return out
    with urllib.request.urlopen(FONT_URL, timeout=120) as r:
        archive = r.read()
    if sha256(archive) != FONT_ARCHIVE_SHA256:
        sys.exit(f"ansi_to_png.py: {FONT_URL} does not have the pinned SHA-256")
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:bz2") as t:
        for k, (member, digest) in FONTS.items():
            data = t.extractfile(member).read()
            if sha256(data) != digest:
                sys.exit(f"ansi_to_png.py: {member} does not have the pinned SHA-256")
            out[k].write_bytes(data)
    return out


def color256(n):
    if n < 16:
        return BASIC16[n]
    if n < 232:
        n -= 16
        levels = (0, 95, 135, 175, 215, 255)
        return (levels[n // 36], levels[(n // 6) % 6], levels[n % 6])
    v = 8 + 10 * (n - 232)
    return (v, v, v)


def parse(text):
    """Rows of cells (char, fg, bg, bold, underline), from SGR-coded text."""
    rows, row = [], []
    fg, bg, bold, under, inverse = None, None, False, False, False
    i = 0
    while i < len(text):
        m = CSI.match(text, i)
        if m:
            i = m.end()
            if m.group(2) != "m":
                continue
            params = [int(p) if p else 0 for p in m.group(1).split(";")] if m.group(1) else [0]
            j = 0
            while j < len(params):
                p = params[j]
                if p == 0:
                    fg, bg, bold, under, inverse = None, None, False, False, False
                elif p == 1:
                    bold = True
                elif p in (2, 22):
                    bold = False if p == 22 else bold
                elif p == 4:
                    under = True
                elif p == 24:
                    under = False
                elif p == 7:
                    inverse = True
                elif p == 27:
                    inverse = False
                elif 30 <= p <= 37:
                    fg = BASIC16[p - 30]
                elif 90 <= p <= 97:
                    fg = BASIC16[p - 90 + 8]
                elif 40 <= p <= 47:
                    bg = BASIC16[p - 40]
                elif 100 <= p <= 107:
                    bg = BASIC16[p - 100 + 8]
                elif p == 39:
                    fg = None
                elif p == 49:
                    bg = None
                elif p in (38, 48) and j + 1 < len(params):
                    if params[j + 1] == 2 and j + 4 < len(params):
                        c = tuple(params[j + 2:j + 5])
                        j += 4
                    elif params[j + 1] == 5 and j + 2 < len(params):
                        c = color256(params[j + 2])
                        j += 2
                    else:
                        c = None
                    if p == 38:
                        fg = c
                    else:
                        bg = c
                j += 1
            continue
        ch = text[i]
        i += 1
        if ch == "\r":
            continue
        if ch == "\n":
            rows.append(row)
            row = []
            continue
        f, b = fg or INK, bg or BG
        if inverse:
            f, b = b, f
        row.append((ch, f, b, bold, under))
        if unicodedata.east_asian_width(ch) in ("W", "F"):
            row.append(("", f, b, bold, under))
    if row:
        rows.append(row)
    return rows


def block_rects(ch, x, y, w, h):
    """The filled part of a block element as (x0, y0, x1, y1) boxes, or None for a character drawn by the font."""
    o = ord(ch)
    if o == 0x2588:
        return [(x, y, x + w, y + h)]
    if o == 0x2580:
        return [(x, y, x + w, y + h // 2)]
    if o == 0x2584:
        return [(x, y + h // 2, x + w, y + h)]
    if o == 0x258C:
        return [(x, y, x + w // 2, y + h)]
    if o == 0x2590:
        return [(x + w // 2, y, x + w, y + h)]
    if 0x2581 <= o <= 0x2587:   # lower one eighth .. seven eighths
        part = round(h * (o - 0x2580) / 8)
        return [(x, y + h - part, x + w, y + h)]
    return None


def render(src, dst):
    need_pillow()
    from PIL import Image, ImageDraw, ImageFont
    fonts = {k: ImageFont.truetype(str(p), FONT_PX) for k, p in font_files().items()}
    ascent, descent = fonts["regular"].getmetrics()
    cw, ch_ = round(fonts["regular"].getlength("M")), ascent + descent
    rows = parse(Path(src).read_text(encoding="utf-8"))
    cols = max((len(r) for r in rows), default=0)
    img = Image.new("RGB", (cols * cw + 2 * PAD, len(rows) * ch_ + 2 * PAD), BG)
    d = ImageDraw.Draw(img)
    for r, row in enumerate(rows):
        y = PAD + r * ch_
        for c, (char, fg, bg, bold, under) in enumerate(row):
            x = PAD + c * cw
            d.rectangle((x, y, x + cw - 1, y + ch_ - 1), fill=bg)
            if not char or char == " ":
                continue
            rects = block_rects(char, x, y, cw, ch_)
            if rects:
                for x0, y0, x1, y1 in rects:
                    d.rectangle((x0, y0, x1 - 1, y1 - 1), fill=fg)
            else:
                d.text((x, y), char, font=fonts["bold" if bold else "regular"], fill=fg)
            if under:
                d.line((x, y + ascent + 1, x + cw - 1, y + ascent + 1), fill=fg)
    img.save(dst, optimize=True)


def optimize(path):
    need_pillow()
    from PIL import Image
    with Image.open(path) as im:
        im.load()
        img = im.convert("RGBA")
    if img.getextrema()[3][0] == 255:
        img = img.convert("RGB")
    colors = img.getcolors(256)
    if colors:   # 256 colours or fewer: a palette image is lossless
        index = {c: i for i, (_, c) in enumerate(colors)}
        pal = Image.new("P", img.size)
        flat = []
        for _, c in colors:
            flat += list(c[:3])
        pal.putpalette(flat + [0] * (768 - len(flat)))
        pal.putdata([index[p] for p in img.getdata()])
        if img.mode == "RGBA":
            pal.info["transparency"] = bytes(c[3] for _, c in colors)
        img = pal
    out = io.BytesIO()
    img.save(out, format="PNG", optimize=True)   # no pnginfo: no text metadata
    Path(path).write_bytes(out.getvalue())


def main(argv):
    if argv and argv[0] == "--optimize":
        for p in argv[1:]:
            optimize(p)
        return 0
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    render(argv[0], argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
