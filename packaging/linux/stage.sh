# Aether Serial Bridge: install, update or uninstall the bridge service.
# asb-stage (v1.1 D60, INV-122, INV-124): the Linux window runs this as root through
#   pkexec /bin/sh -c <this script> asb-stage <sha256> <program length> <request length>
# with the program's bytes, then the request, on stdin. It copies them into a fresh root-only directory, checks the
# program's SHA-256 against the digest it was given, and only then runs the copy as --helper with that request.
# Exits: the staged helper's 0/1/2/5 pass through; 3 digest mismatch; 4 arguments, framing, an unsafe directory,
# staging I/O or a signal; 6 the copy could not be executed. 3, 4 and 6 mean nothing ran and nothing changed, and
# each prints exactly one "asb-stage:" line on stderr first. Embedded byte for byte in the window (src/cli).
# SC2015: every `A && B || die` here means "die unless A and B"; die always exits.
# shellcheck shell=sh disable=SC2015
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH
die() { echo "asb-stage: $2" >&2; exit "$1"; }
[ $# -eq 3 ] || die 4 "bad arguments"   # before set -u could exit 2 on a missing one
want=$1 plen=$2 rlen=$3
case $want in (*[!0-9a-f]*|'') die 4 "bad digest" ;; esac; [ ${#want} -eq 64 ] || die 4 "bad digest"
for n in "$plen" "$rlen"; do case $n in (''|0*|*[!0-9]*) die 4 "bad lengths" ;; esac; done   # no leading 0: octal
[ ${#plen} -le 9 ] && [ "$plen" -le 268435456 ] && [ ${#rlen} -le 6 ] && [ "$rlen" -le 262144 ] || die 4 "bad lengths"
for d in / /usr /usr/local /usr/local/lib; do   # whoever can write an ancestor can swap the staging root after the check
  [ -d "$d" ] && [ ! -L "$d" ] && m=$(stat -c %u:%a "$d") || die 4 "$d is not a directory only root can write"
  case $m in (0:*[2367][0-7]|0:*[2367]|[!0]*) die 4 "$d is not a directory only root can write" ;; esac
done
root=/usr/local/lib/aether-serial-bridge
if [ -e "$root" ] || [ -L "$root" ]; then
  [ -d "$root" ] && [ ! -L "$root" ] && [ "$(stat -c %u:%g:%a "$root")" = 0:0:700 ] ||
    die 4 "the staging root is not a root-only directory"
else
  # A set-group-ID parent would hand the new directory its bit and its group: make it root:root 0700 like the check.
  { mkdir -m 0700 "$root" && chown 0:0 "$root" && chmod g-s "$root"; } || die 4 "cannot create the staging root"
fi
dir=$(mktemp -d "$root/staging.XXXXXXXX") || die 4 "cannot create a staging directory"
trap 'rm -rf -- "$dir"' EXIT
trap 'rm -rf -- "$dir"; echo "asb-stage: interrupted" >&2; exit 4' HUP INT TERM
head -c $((plen + rlen + 1)) > "$dir/payload" || die 4 "cannot store the payload"
size=$(stat -c %s "$dir/payload") || die 4 "cannot store the payload"
[ "$size" -eq $((plen + rlen)) ] || die 4 "the payload is not $plen+$rlen bytes"
head -c "$plen" "$dir/payload" > "$dir/aether-serial-bridge" || die 4 "cannot split the payload"
tail -c "+$((plen + 1))" "$dir/payload" > "$dir/request.json" || die 4 "cannot split the payload"
rm -f -- "$dir/payload" || die 4 "cannot split the payload"
got=$(sha256sum < "$dir/aether-serial-bridge") || die 4 "cannot hash the program"; got=${got%% *}
[ "$got" = "$want" ] || die 3 "the program does not match its digest"
chmod 0700 "$dir/aether-serial-bridge" || die 4 "cannot make the program executable"
trap : HUP INT TERM   # from here the program may change things: its own exit code must reach the window
# Through env: a copy that cannot be executed is reported by env (not by this shell, whose messages start with $0,
# "asb-stage:"), and a wrong-architecture file is never run as a shell script (the shell's ENOEXEC fallback).
rc=0; env "$dir/aether-serial-bridge" --helper < "$dir/request.json" || rc=$?
case $rc in (126|127) die 6 "the staged program could not be executed ($rc)" ;; esac
exit "$rc"
