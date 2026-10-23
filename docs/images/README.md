# Documentation images

The images the operator docs show. Every image here is rendered from the demo fixture in [`demo/`](demo/), so no
image carries a real host name, address, device serial or callsign (INV-135). Only the documentation address ranges
(`192.0.2.0/24`, `198.51.100.0/24`, `203.0.113.0/24`, `2001:db8::/32`), loopback and the wildcard addresses appear.

| Folder | What | Made by |
|---|---|---|
| `windows/` | the window (on the native `windows` platform: offscreen there has no font fallback for the symbols Inter lacks) and the TUI from the Windows test build | `docs-shots.yml`, job `windows` (windows-latest) |
| `linux/` | the window (offscreen) and the TUI from the Linux x64 test build | `docs-shots.yml`, job `linux` (the Ubuntu 22.04 build container) |
| `pi/` | the TUI from the Raspberry Pi's arm64 build | `docs-shots.yml`, job `pi` (a hosted arm64 runner) |
| `<os>/manual/` | what cannot be rendered: UAC, the Settings app, the polkit dialog, the Pi's desktop | a person, by hand |
| `demo/` | the fixture: two ports ("Amp" on 4001, "Tuner" on 4002), the status with one client, the installations, the machine's devices and addresses, and the gate's allow-list | |

## Names

`<what>-<state>[-<theme>][@2x].png`, and beside each rendered PNG a manifest `<name>.json`: the OS, the state, the
theme, the scale, the size in pixels and every text the image shows (labels, buttons, fields, list and table cells,
headers, the title; for the TUI, every line of the frame). Windows are rendered at their launch size, 1040 × 760, in
AetherSDR's Default Dark or Default Light; `@2x` is the same window at scale 2 (the README's image). A question is
rendered as its own widget, at least 760 pixels wide so its lines are not cut. TUI frames are FTXUI's true-colour
output for the fixture, drawn in DejaVu Sans Mono 2.37 on the TUI's aethersdr.com palette.

## Regenerate

1. `gh workflow run docs-shots.yml --ref <branch>`.
2. Download each `docs-shots-<os>` artifact into an empty folder and look at every image: the right state, the right
   theme, nothing cut off, nothing that is not in the fixture.
3. Copy them into `<os>/`, run `python3 tools/check_docs_images.py`, and commit.

Locally (Linux): build `tst_BridgeWindow` and `tst_Tui` with the GUI on, then
`python3 tools/docs_shots.py --os linux --bin <build>/bin --out <empty folder>` with Pillow 11.0.0 installed. The
generator refuses any fixture but this folder's `demo/`, points `HOME`, `XDG_*`, `APPDATA` and `LOCALAPPDATA` at a
temporary folder, and refuses an image without its manifest; the hooks (`tst_BridgeWindow::docsShots`,
`tst_Tui::docsShots`) refuse to run without both.

## The gate

`tools/check_docs_images.py` runs in `static-checks`. It fails when a rendered PNG has no manifest, when a manifest
string (any field) holds an address outside the ranges above (an IPv6 address is read whole, and a token that looks
like one but does not parse fails too), a word shaped like an amateur callsign (one or two letters, or a digit and one
or two letters, then a digit and one to four letters, in either case: `CALLSIGN` in `tools/check_docs_images.py`), or
a path or host name that `demo/allow-list.json` does not list (on whole path components), when any PNG (the manual
ones too) carries text metadata, or when this folder reaches 4 MB. The fixture files are held to the same rules.

## Hand-captured images (`<os>/manual/`)

Captured by hand on a test machine (never a station in use), as a user named `demo`. Before committing one, a
person checks it against this list:

- [ ] Cropped to the dialog or window: no taskbar, no desktop icons, no other windows, no clock.
- [ ] No real user name (only `demo`), host name, address, device serial, callsign or email address. Ports and
      devices, when shown, are the fixture's.
- [ ] No notification, wallpaper or browser tab that says anything about the machine or its owner.
- [ ] Metadata stripped: re-saved with Pillow (`Image.open(p).save(p, optimize=True)`); the gate refuses text chunks.
- [ ] Under 300 KB.

The docs name these images in their text until they exist, so no page shows a broken image.
