#!/usr/bin/env bash
# Install ser2net and drop in the aether-serial-bridge example config.
# Debian / Raspberry Pi OS / Ubuntu. Run with sudo.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
conf=/etc/ser2net.yaml

if [[ $EUID -ne 0 ]]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi

apt-get update
apt-get install -y ser2net

if [[ -e "$conf" ]] && ! cmp -s "$conf" "$here/ser2net/ser2net.yaml"; then
    backup="$conf.bak.$(date +%Y%m%d%H%M%S)"
    cp -a "$conf" "$backup"
    echo "existing $conf saved to $backup"
fi
install -m 0644 "$here/ser2net/ser2net.yaml" "$conf"

systemctl enable ser2net >/dev/null

echo
echo "Serial adapters on this host:"
if compgen -G '/dev/serial/by-id/*' >/dev/null; then
    ls -l /dev/serial/by-id/
else
    echo "  none found. Plug in the USB-serial cables and run: ls -l /dev/serial/by-id/"
fi

echo
echo "Next:"
echo "  1. sudoedit $conf. Replace each REPLACE-ME path, check baud rates, set a LAN/VPN bind address."
echo "  2. sudo systemctl restart ser2net"
echo "  3. ss -ltn | grep ser2net   (or: ss -ltnp) to confirm the ports are listening"
