# Aether Serial Bridge or ser2net?

[ser2net](https://github.com/cminyard/ser2net) is a long-standing Linux daemon that does the same basic job
as Aether Serial Bridge: it gives each serial port its own TCP port. Both work with AetherSDR's network
mode. This page says honestly where each one is the better tool.

## Use ser2net when

- **The device needs RFC 2217**, meaning telnet COM-port control: the client changes the baud rate and
  drives DTR/RTS on the real port. RFC 2217 and remote DTR/RTS control are **not goals** of Aether Serial
  Bridge, and it doesn't do them. The main case is the SPE Expert amplifiers: AetherSDR drives DTR/RTS
  over RFC 2217 for remote power-on.
- **The host can't run the bridge.** That covers a 32-bit OS (such as 32-bit Raspberry Pi OS), glibc older
  than 2.35, a host without systemd or with cgroup v1, or an ordinary container. The bridge needs all of
  these ([linux.md](linux.md#which-file)). ser2net is packaged by Debian, Ubuntu and Raspberry Pi OS.
- **You already run ser2net and it works.** There is no need to switch.
- **You want ser2net's other features.** It has many more connection options than this bridge; see its
  own documentation.

## Use Aether Serial Bridge when

- **The serial gear is cabled to a Windows PC.** ser2net is a Linux daemon. The bridge runs as a Windows
  service with a configuration window, and it writes the Windows firewall rules for each port and its
  allowed clients.
- **You want setup without a config file:** a window (Windows, x64 Linux desktop), a terminal interface
  that works over SSH, or the port commands. Each edit is checked before it is saved.
- **You want the bridge's safety rules:**
  - it is raw only, so a binary protocol (ACOM) can't be broken by a wrong telnet setting;
  - DTR and RTS are fixed per port, off by default, and no client can toggle them;
  - a change is applied as one guarded operation, and a failed apply leaves the bridge off rather than
    running half-applied settings ([architecture.md](architecture.md#configuration-files-and-the-apply-model)).
- **You want a status view:** `--status`, the window and the TUI show each port's state, the connected
  client, byte counts and the last error.

Both tools let a new connection take over a port from a stale one: ser2net with `kickolduser: true`, the
bridge always. Neither has a password. Whoever can reach a port controls the device, so bind both to a LAN
or VPN address and firewall them.

## Quick comparison

| | Aether Serial Bridge | ser2net |
|---|---|---|
| Raw TCP | yes, always | yes (`accepter: tcp,...`) |
| Telnet / RFC 2217 (remote baud, DTR, RTS) | **no** (not a goal) | yes (`accepter: telnet(rfc2217),tcp,...`) |
| Windows | yes: service, window, firewall rules | no |
| Linux | 64-bit (x64, arm64), glibc 2.35+, systemd with cgroup v2 | packaged by the major distributions |
| Configuration | window, terminal interface, command line | `/etc/ser2net.yaml` |
| Allowed-client list | Windows: enforced by the firewall rules it writes. Linux: recorded, not enforced. | use your firewall |

## Running both

You can run both on one machine, as long as each serial port and each TCP port belongs to only one of them.
A common setup is the bridge for the Elecraft and ACOM gear, and ser2net for an SPE.

## ser2net with this repository

The repository still carries a ready-made ser2net setup for Debian, Raspberry Pi OS and Ubuntu. It is
separate from the bridge program.

- [`ser2net/ser2net.yaml`](../ser2net/ser2net.yaml): an example ser2net 4.x configuration with one block per
  device. The Elecraft devices are active. ACOM (raw) and SPE (telnet + RFC 2217) are commented out.
- [`scripts/install.sh`](../scripts/install.sh): installs ser2net with `apt-get` and copies the example
  configuration to `/etc/ser2net.yaml`. It backs up an existing file first. It also enables the ser2net
  service and lists your serial adapters.

```bash
git clone https://github.com/AetherSDR/AetherSerialBridge
cd aether-serial-bridge
sudo ./scripts/install.sh          # installs ser2net, drops in the example config, lists your adapters
sudoedit /etc/ser2net.yaml         # set the by-id paths, baud rates and bind address
sudo systemctl restart ser2net
ss -ltn                            # the configured TCP ports should be listening
```

For each device you use in `ser2net.yaml`:

1. Replace the `REPLACE-ME` path with the adapter's `/dev/serial/by-id/...` name (`ls -l /dev/serial/by-id/`).
   Don't use `/dev/ttyUSB0`: the numbers can swap after a reboot.
2. Check that the baud rate matches the device's own setting ([devices.md](devices.md)).
3. Bind to a LAN or VPN address: change `accepter: tcp,4001` to `accepter: tcp,192.168.1.50,4001`. Without
   an address, ser2net listens on every interface.

| Device | Example port | ser2net mode | Why |
|---|---|---|---|
| Elecraft KAT500 | 4001 | raw | ASCII protocol |
| Elecraft KPA500 | 4002 | raw | ASCII caret protocol |
| Elecraft KXPA100 | 4003 | raw | ASCII caret protocol |
| ACOM S-series | 4010 | **raw (required)** | Telnet mode doubles byte `0xFF`, which ACOM's binary protocol uses as data |
| SPE Expert | 4011 | **telnet + RFC 2217 (required)** | AetherSDR drives DTR/RTS remotely for power-on |

`kickolduser: true` on every port gives the bridge's behaviour: a new connection replaces the old one, so
a client that crashed without closing its socket can't lock the port. The `local` option on the raw ports
tells ser2net to ignore the modem-control lines, so a missing DCD signal doesn't stop the data.

Test a raw port (a KAT500, asking for the firmware version):

```bash
printf 'RV;' | nc -w 2 192.168.1.50 4001      # expect a reply like RV02.16;
```

ser2net logs to the journal: `journalctl -u ser2net -f`. For firewall examples (ufw, firewalld, nftables),
see [linux.md](linux.md#firewall).
