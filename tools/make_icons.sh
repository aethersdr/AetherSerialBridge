#!/usr/bin/env bash
# Rebuilds the application icon set in resources/icon/ from its master (ImageMagick 6 or 7):
#
#   asb-icon-master-1024.png   the master: a navy disc with the cyan "A" waveform rising out of an RS-232 DB9 port,
#                              transparent corners. The only input; everything else below is generated from it.
#   asb-icon-<N>.png           N = 16 32 48 64 128 256 512 (the window icon set in resources/asb.qrc, the AppImage)
#   asb.ico                    16 20 24 32 40 48 64 128 256 (the 256 frame PNG-compressed): both Windows executables,
#                              through asb.rc
#
# 64 px and up are Lanczos reductions of the master. 16, 20, 24, 32, 40 and 48 are redrawn as crisp vector strokes of
# the same motif (step 3 says how and what each one leaves out): the master's double strokes vanish at those sizes.
#
# Usage: tools/make_icons.sh [--sheet <contact-sheet.png>]
#   --sheet  also writes a contact sheet: every size at 1:1 and at 4x (nearest neighbour), on dark (#1e1e1e) and
#            light (#f3f3f3) backgrounds.
# Output is byte-for-byte reproducible for a given ImageMagick version (no timestamps: -strip).
set -euo pipefail

sheet=""
case "${1:-}" in
    "") ;;
    --sheet) sheet="${2:?--sheet needs a path}"; [[ "$sheet" = /* ]] || sheet="$PWD/$sheet" ;;
    *) echo "usage: $0 [--sheet <contact-sheet.png>]" >&2; exit 2 ;;
esac

if command -v magick >/dev/null 2>&1; then IM=(magick); else IM=(convert); fi
root="$(cd "$(dirname "$0")/.." && pwd)"
dir="$root/resources/icon"
master="$dir/asb-icon-master-1024.png"
[ -f "$master" ] || { echo "make_icons: no master at $master" >&2; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

out() { "${IM[@]}" "$@"; }   # every PNG below is written -strip PNG32: (RGBA, no date or text chunks)

# 1. The master, normalised: the disc trimmed out of its canvas and centred on 1024 with an 8 px margin.
out "$master" -trim +repage -resize 1008x1008 -background none -gravity center -extent 1024x1024 \
    -strip "PNG32:$tmp/norm.png"

# 2. 64 px and up: Lanczos reductions; at 64 a light unsharp mask keeps the strokes from going soft.
for n in 64 128 256 512; do
    sharpen=()
    [ "$n" -le 64 ] && sharpen=(-unsharp 0x0.6+0.8+0.01)
    out "$tmp/norm.png" -filter Lanczos -resize "${n}x${n}" "${sharpen[@]}" -strip "PNG32:$tmp/$n.png"
done

# 3. 16 to 48 are redrawn, not reduced: the master's strokes are under a pixel there and its double
#    lines and the connector's nested outlines fuse into blobs. The same motif is drawn as vector strokes, in the
#    master's coordinates (1024 square, measured off it), at 8x the target size, then box-reduced: the strokes are a
#    set width in target pixels (about 1 to 2 px) and the disc's edge is crisp. Simplifications, by size:
#      all small sizes  the A is one stroke (not the master's double line), its tails trimmed; the second wave is
#                       one thinner, dimmer stroke; the view is the centre 860 of 1024 (the art a little larger)
#      16, 20, 24       the connector is its D-shaped face plus the two mounting ears; no shell, no pins, no screws
#      32, 40, 48       the connector's shell, face and nine pins; no screw holes
A_PATH=$(awk 'BEGIN { for (x = 330; x <= 694; x += 4) {
    y = 548 - 358 * exp(-((x - 512) / 95) ^ 2 / 2); printf "%s%d,%.1f", (x == 330 ? "M " : " L "), x, y } }')
WAVE_PATH=$(awk 'BEGIN { pi = 3.14159265; for (x = 150; x <= 880; x += 10) {
    y = 392 - 50 * sin(2 * pi * (x - 470) / 620); printf "%s%d,%.1f", (x == 150 ? "M " : " L "), x, y } }')
SHELL_PATH='M 270,612 L 340,612 L 360,585 L 664,585 L 684,612 L 754,612 Q 778,612 778,636 L 778,782 Q 778,806 754,806 L 684,806 L 664,833 L 360,833 L 340,806 L 270,806 Q 246,806 246,782 L 246,636 Q 246,612 270,612 Z'
FACE_PATH='M 385,632 L 639,632 Q 668,632 663,660 L 645,772 Q 641,796 616,796 L 408,796 Q 383,796 379,772 L 361,660 Q 356,632 385,632 Z'

# small <size> <A width> <wave width> <connector width> <detail: ears|full>   (widths in target pixels)
small() {
    local n=$1 big=$(( $1 * 8 )) view=860
    local s; s=$(awk -v b="$big" -v v="$view" 'BEGIN { printf "%.6f", b / v }')
    local t; t=$(awk -v s="$s" -v v="$view" 'BEGIN { printf "%.3f", -(512 - v / 2) * s }')
    px() { awk -v w="$1" -v v="$view" -v n="$n" 'BEGIN { printf "%.2f", w * v / n }'; }   # target px -> master units
    local mvg="affine $s,0,0,$s,$t,$t fill none stroke-linecap round stroke-linejoin round
        stroke '#3fb4d4' stroke-width $(px "$3") path '$WAVE_PATH'
        stroke '#b8faff' stroke-width $(px "$2") path '$A_PATH'
        stroke '#36b2ea' stroke-width $(px "$4") path '$FACE_PATH'"
    if [ "$5" = ears ]; then
        mvg+=" line 262,714 345,714 line 679,714 762,714"
    else
        mvg+=" path '$SHELL_PATH' stroke none fill '#6fd8f5'"
        local r; r=$(px 0.55)
        for x in 415 463 511 558 605; do mvg+=" circle $x,680 $(awk -v x=$x -v r="$r" 'BEGIN { print x + r }'),680"; done
        for x in 438 487 533 582; do mvg+=" circle $x,735 $(awk -v x=$x -v r="$r" 'BEGIN { print x + r }'),735"; done
    fi
    local c=$(( big / 2 ))
    out -size "${big}x${big}" xc:none -fill '#14295a' -stroke '#08132b' -strokewidth $(( big / 24 )) \
        -draw "circle $c,$c $c,$(( big / 48 ))" -strip "PNG32:$tmp/disc$n.png"
    out -size "${big}x${big}" xc:none -draw "$mvg" -strip "PNG32:$tmp/art$n.png"
    out "$tmp/disc$n.png" "$tmp/art$n.png" -compose Over -composite "$tmp/disc$n.png" -compose DstIn -composite \
        -filter Box -resize "${n}x${n}" -strip "PNG32:$tmp/$n.png"
}
small 16 1.5 0.9 1.2 ears
small 20 1.6 1.0 1.3 ears
small 24 1.8 1.1 1.4 ears
small 32 2.0 1.3 1.4 full
small 40 2.2 1.4 1.5 full
small 48 2.4 1.5 1.6 full

# 4. The committed set and the .ico (frames in this order, each one exactly the PNG above; IM writes a 256 frame
#    PNG-compressed).
for n in 16 32 48 64 128 256 512; do cp "$tmp/$n.png" "$dir/asb-icon-$n.png"; done
ico=()
for n in 16 20 24 32 40 48 64 128 256; do ico+=("$tmp/$n.png"); done
out "${ico[@]}" "$dir/asb.ico"

# 5. Optional contact sheet: rows 1:1 and 4x, columns per size, dark half then light half.
if [ -n "$sheet" ]; then
    sizes=(16 20 24 32 40 48 64 128)
    for bg in '#1e1e1e' '#f3f3f3'; do
        for scale in 1 4; do
            row=()
            for n in "${sizes[@]}"; do
                cell=$(( n * scale + 16 ))
                out "$tmp/$n.png" -filter Point -resize "$(( n * scale ))x$(( n * scale ))" \
                    -background "$bg" -gravity center -extent "${cell}x${cell}" "$tmp/c-$scale-$n-${bg#\#}.png"
                row+=("$tmp/c-$scale-$n-${bg#\#}.png")
            done
            out "${row[@]}" -background "$bg" -gravity center +append "$tmp/r-$scale-${bg#\#}.png"
        done
        out "$tmp/r-1-${bg#\#}.png" "$tmp/r-4-${bg#\#}.png" -background "$bg" -gravity center -append \
            "$tmp/half-${bg#\#}.png"
    done
    out "$tmp/half-1e1e1e.png" "$tmp/half-f3f3f3.png" -background '#808080' -gravity center -append \
        -strip "PNG32:$sheet"
    echo "contact sheet: $sheet"
fi
echo "icons written to $dir"
