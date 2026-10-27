# zeta-iwi-wifi

**WPA2 Wi-Fi for BeOS ZETA on the Intel PRO/Wireless 2200BG ("Centrino b/g") — a from-scratch kernel driver and WPA2 supplicant.**

ZETA 1.2.1 (2006) has no usable driver for the Intel PRO/Wireless 2200BG mini-PCI card found in many 2004–2006 laptops
(Dell Inspiron 510m/600m/700m/9200/..., IBM/Lenovo ThinkPads, ...). Its built-in NDIS wrapper panics the kernel on this
card, and ZETA has no WPA at all. This project fixes both:

* a small **kernel driver** (`iwi`) that boots the card's firmware and exposes it to the BONE network stack as an ordinary
  Ethernet device, `/dev/net/iwi/0`;
* a userland **daemon** (`iwid`) that does everything the firmware does not: the **WPA2-PSK** four-way handshake,
  **CCMP (AES) encryption/decryption in software**, **DHCP**, default route and DNS, auto-reconnect and a firmware watchdog;
* a boot script, so the machine simply has Wi-Fi after power-on.

Written in an evening-to-night of reverse-engineering and debugging on a real Dell Inspiron 510m running ZETA 1.2.1 —
see [docs/10-debugging-story.md](docs/10-debugging-story.md) for how it went (including the firmware crash that took the
longest to find).

```
  ZETA apps ── BONE network stack ── /dev/net/iwi/0 ┐
                                                      │  Ethernet frames
                                     iwid (userland) ─┤  WPA2 handshake, CCMP, DHCP, reconnect
                                                      │  raw 802.11 frames + firmware commands
                               /dev/misc/iwi/0 ── iwi (kernel driver, thin) ── Intel 2200BG + Intel firmware
```

## Status

Verified on real hardware: **Dell Inspiron 510m, Intel PRO/Wireless 2200BG (PCI 8086:4220 rev 05), ZETA 1.2.1.**

| | |
|---|---|
| Boots the Intel firmware, scans, joins a **WPA2-PSK (CCMP)** network | works |
| DHCP lease, default route and DNS taken from the router | works |
| `ping` router / internet / names from ZETA over Wi-Fi (≈ 8 ms / 17 ms / 33 ms) | works |
| ZETA reachable from other machines on the Wi-Fi (ping, SSH) | works |
| Builds from a clean copy with ZETA's own gcc 2.95; `make test` passes on the machine itself | works |
| Starts by itself at every boot (~30 s after power-on), restarts if it dies | works |
| Firmware-crash watchdog (restarts the card automatically) | implemented, never needed yet |
| Reconnect after the access point drops us | implemented, **not** tested for real |
| WPA (TKIP), WEP, open networks, 802.11a (2915ABG), ad-hoc, power save, roaming | **not supported** |
| Other cards (4221 / 4223 / 4224), other ZETA versions, BeOS R5, Haiku | **untested** |

Throughput has not been benchmarked. The driver is polled (no interrupts) and the cryptography is plain C on a
1.6 GHz Pentium M — fine for browsing and file transfers, not a speed demon.

## Quick start (on the ZETA machine)

You need ZETA's developer tools (gcc 2.95 + headers, `make`) and a **WPA2-PSK / CCMP** network.

```sh
# 1. build
cd zeta-iwi-wifi
make test            # optional: runs the crypto self-test on the machine itself
make                 # builds ./iwi (kernel driver) and ./iwid (daemon)

# 2. install the driver and daemon (nothing changes at boot yet)
sh scripts/install.sh

# 3. Intel's firmware (not included, see below): shows you Intel's licence and asks you to accept it
sh scripts/get-firmware.sh

# 4. your network
edit /boot/home/config/settings/iwi.conf        # ssid=...  psk=...

# 5. try it
/boot/home/ZetaWifi/iwid --scan                 # lists access points
/boot/home/ZetaWifi/iwid -b -r -v               # connect, DHCP, route+DNS; Ctrl-C to stop

# 6. once it works: start Wi-Fi at every boot
sh scripts/install.sh --boot
```

Full instructions, flags, logs and removal: [docs/08-install-and-usage.md](docs/08-install-and-usage.md).

## Intel's firmware is NOT included

The card needs Intel's proprietary firmware (a ~190 KB file). Its licence allows use only with Intel hardware, forbids
modification, and requires the licence text to accompany it — so it cannot live in this repository.
`scripts/get-firmware.sh` downloads it from OpenBSD's firmware server, shows you the licence, and installs it only if you
type `yes`. Details: [docs/05-firmware.md](docs/05-firmware.md).

## Documentation

| | |
|---|---|
| [01 Overview](docs/01-overview.md) | what this is, why it is built this way, the big picture |
| [02 Hardware and environment](docs/02-hardware-and-environment.md) | the card, the laptop, ZETA, the toolchain, how to identify your card |
| [03 Architecture](docs/03-architecture.md) | components, data paths, memory, locking, boot flow |
| [04 Kernel driver](docs/04-kernel-driver.md) | the driver in detail: reset, firmware loading, rings, TX/RX, ioctls, BONE face |
| [05 Firmware](docs/05-firmware.md) | getting Intel's firmware, its format, the licence |
| [06 WPA2 daemon](docs/06-wpa2-daemon.md) | scan, associate, EAPOL handshake, CCMP, DHCP, bridge, watchdog |
| [07 Network integration](docs/07-network-integration.md) | how it plugs into BONE: interfaces, routes, DNS, what the stack expects |
| [08 Install and usage](docs/08-install-and-usage.md) | build, install, configure, run, boot hook, uninstall |
| [09 Troubleshooting](docs/09-troubleshooting.md) | symptoms, causes, how to read the logs and the firmware error log |
| [10 Debugging story](docs/10-debugging-story.md) | every bug found, in order, and how |
| [11 Reference](docs/11-reference.md) | registers, structures, command and notification numbers, ioctls, packet layouts |
| [12 Security notes](docs/12-security.md) | what is and is not protected |
| [13 Roadmap](docs/13-roadmap.md) | what is missing and how to help |
| [Sample log](docs/sample-log.md) | what a healthy run looks like |

## Repository layout

```
src/kernel/iwi.c          the kernel driver (one file)
src/include/iwi_ioctl.h   interface between driver and daemon
src/daemon/iwid.c         the daemon: scan, join, WPA2, DHCP, bridge, watchdog
src/daemon/wpa.c, wpa.h   SHA-1, HMAC, PBKDF2, PRF, AES-128, key unwrap, CCM (no external libraries)
scripts/                  install.sh, get-firmware.sh, start-wifi.sh (boot starter)
config/iwi.conf.example   configuration template
tests/test_wpa.c          crypto self-test against published vectors
tests/ethtest.c           asks /dev/net/iwi/0 for its MAC address (checks the BONE face)
docs/                     the documentation
```

## Credits and licence

ISC licence — see [LICENSE](LICENSE). This work stands on:

* **OpenBSD `iwi(4)`** by Damien Bergamini (ISC) — the register map, firmware loading sequence, descriptor and command formats.
  FreeBSD's `iwi(4)` (same author) was used to cross-check details (e.g. the sensitivity command).
* **Linux `ipw2200`** — reading how the firmware reports its own crashes (error-log layout). No code was copied.
* **Haiku**'s open-source BeOS-style Ethernet drivers (`wb840`) and driver headers — as a reference for the driver API.
* **Be Inc. / yellowTAB / ZETA** — the OS, headers, and the `eepro100` driver whose disassembly confirmed the exact
  BONE ioctl numbers.
* **Intel** — the firmware (not distributed here).

"BeOS", "ZETA" and "Intel" are trademarks of their owners. This project is not affiliated with or endorsed by any of them.
Use at your own risk: it is a kernel driver written for a hobby machine; it has been tested on exactly one laptop.
