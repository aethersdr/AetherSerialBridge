# Using the bridge from software that only opens COM ports

AetherSDR connects to the bridge directly and needs none of this. These recipes are for vendor utilities
and loggers that can only open a local serial port. Each one creates a virtual serial port that forwards to
a TCP port on the bridge.

Three things to keep in mind:

- **One client per port.** Disconnect AetherSDR before you open the vendor utility, and the other way
  round. A new connection replaces the current one.
- **The bridge port is raw TCP.** Set the virtual-port tool to a plain TCP client. Don't use telnet or
  RFC 2217 mode against an Aether Serial Bridge port. Telnet framing would corrupt binary protocols such as
  ACOM's.
- **The baud rate is set on the bridge, not in the program.** The program's baud, parity, DTR and RTS
  settings stay on the virtual port and never reach the real one. Set the serial settings, and DTR/RTS if
  the device needs them, on the bridge's port ([devices.md](devices.md)).

## Linux

`socat` creates a pseudo-terminal linked to the bridge:

```bash
socat -d pty,link=$HOME/.ttyKAT500,raw,echo=0 tcp:<bridge-host>:4001
```

Point the program at `~/.ttyKAT500`.

## macOS

The same `socat` command works (`brew install socat`). Some programs only list the ports macOS itself
reports, so they won't show the pseudo-terminal. Those need a program that lets you type a device path.

## Windows

- **com0com + hub4com** (free, open source): com0com creates a pair of virtual COM ports, and hub4com
  connects one end to the bridge over raw TCP. Point the program at the other end of the pair.
- **Commercial** virtual-serial-port tools, for example HW VSP3 from HW group or Electronic Team's Serial to
  Ethernet Connector, do the same thing behind a GUI. Choose a raw TCP client connection, without RFC 2217.

## Devices that need RFC 2217

An SPE Expert is bridged with ser2net in RFC 2217 mode, not with Aether Serial Bridge
([ser2net.md](ser2net.md)). For such a port, use an RFC 2217-aware client, such as `ttynvt` on Linux or
hub4com's RFC 2217 mode on Windows. Then the program's baud and DTR/RTS changes reach the real port.
