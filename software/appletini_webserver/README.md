# Appletini 6502 Web Demos

These ProDOS system programs use the Appletini Uthernet II interface in slot 1:

- `A2WEBSRV.SYSTEM` serves a small HTTP/1.0 status page.
- `A2BROWSE.SYSTEM` requests `http://httpbin.io/headers` and displays the HTTP
  status and returned JSON.

Both run on an NMOS 6502 and do not require an enhanced Apple //e.

The programs follow Contiki's working Uthernet II design: the W5100 runs one
MACRAW socket and the 6502 handles ARP, IPv4, and TCP in software. The compact
IP65 stack supplies those protocols and its Uthernet II driver is derived from
Contiki's W5100 driver. Neither program loads `contiki.cfg` or other disk-backed
network settings.

MAC, IP, subnet, and gateway are read from the card before IP65 initializes.
The programs establish a valid W5100 socket-memory map first, then restore the
saved values to both IP65 and the card. Because a W5100 has no DNS register,
the browser asks the saved gateway to resolve `httpbin.io`.

## Build

Install [cc65](https://github.com/cc65/cc65), put `cl65` in `PATH`, then run:

```bat
build.bat
```

The build uses the cc65 `apple2` target, `apple2-system.cfg`, and an explicit
`--cpu 6502`. It creates AppleSingle ProDOS SYS files at
`build\A2WEBSRV.SYSTEM` and `build\A2BROWSE.SYSTEM`, with load address `$2000`
encoded in their metadata. The trimmed, pinned IP65 library and its MPL 1.1
license are in `ip65\`.

## Run

1. Enable the Appletini Ethernet card in slot 1.
2. Enable boot network configuration and select either Static or DHCP in the
   Appletini boot menu.
3. Put either system program on a ProDOS disk using an AppleSingle-aware tool.
4. Launch the server and open its displayed IP address, or launch the browser
   to retrieve the fixed demo URL.
5. Press Escape to stop the server. Both programs wait for a key before
   returning when a result or error needs to be read.

The server accepts one connection at a time and serves `/` and `/index.html`.
The page is embedded in the executable, so no web files are required on disk.

The Contiki reference is the
[official webserver example](https://github.com/contiki-os/contiki/tree/master/examples/webserver).
