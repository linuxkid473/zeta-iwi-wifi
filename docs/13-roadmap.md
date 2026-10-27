# 13. Roadmap and how to help

## Known gaps (roughly in order of usefulness)

1. **Real-world testing on more hardware.** Everything was verified on one Dell Inspiron 510m. The same driver *should* work with the
   2225BG (0x4221) and — on 2.4 GHz — the 2915ABG (0x4223/0x4224), on other ZETA 1.2.x installs, and maybe BeOS R5 + BONE and Haiku's legacy
   driver API, but none of that is tested. **If you try it, please report your PCI id, OS and what happened.**
2. **Reconnect after the AP drops us** is implemented but only exercised on paper. It needs a test with a router that is rebooted /
   moved out of range.
3. **Unexplained drop** — one silent link death ~2 minutes after start during a simulated-boot test (see the debugging story). The watchdog
   should recover from it; it never fired in later runs. A long soak test (days) with `-v` logging would settle it.
4. **WPA (TKIP) and WEP**, **open networks** — the firmware supports them; the daemon only implements WPA2/CCMP. Open networks are easy
   (skip the handshake, no encryption); TKIP needs the Michael MIC and RC4 key mixing.
5. **Receive replay protection** and **PTK rekey** (see [12-security.md](12-security.md)).
6. **Several networks / roaming / SSID list.** The config holds one SSID. A list with priorities and a "connect to the best known one"
   rule would be natural.
7. **Interrupt-driven operation.** The driver polls (~1.5 ms in the daemon). An interrupt handler + a blocking `read()` would cut the idle
   CPU cost to near zero. The main risk is the shared IRQ line; the firmware must be acknowledged correctly.
8. **A proper UI.** A Deskbar replicant / preference panel to pick a network and show signal strength. The daemon already knows everything
   (signal per AP, current link state); it just does not publish it anywhere. A tiny status file or a BMessage port would be enough.
9. **Integration with ZETA's Network preferences** (so the interface shows up as a normal profile) instead of the daemon configuring
   it with `ifconfig`/`route`.
10. **Power saving** (`SET_POWER_MODE` ≠ CAM) for battery life.
11. **TX path details**: use the four TX rings for WMM/QoS, process TX completions (error statistics, `collisions`/`errors` counters in
    `ifconfig`), larger scatter/gather.
12. **5 GHz channels on the 2915ABG** and per-country channel lists (currently channels 1–13 at power 20).
13. **A packaged installer** (`.pkg` / `.zip` with a GUI "Install" app) so non-developers do not need the compiler on the target.

## Test checklist for a new machine

```
 [ ] make test               → "all tests passed"
 [ ] make                    → ./iwi, ./iwid built
 [ ] sh scripts/install.sh   → /dev/misc/iwi/0 and /dev/net/iwi/0 exist
 [ ] sh scripts/get-firmware.sh
 [ ] ./iwid --scan           → lists your network with [RSN]
 [ ] ./iwid                  → "ARP REPLY ... encrypted traffic works in both directions"
 [ ] ./iwid -b -r -v         → DHCP lease, route, DNS; ping router, 1.1.1.1, a name
 [ ] leave it 10+ minutes    → "bridge:" lines keep counting; no "silent"
 [ ] sh scripts/install.sh --boot ; reboot → Wi-Fi up ~30 s after power-on
```

## Contributing

Pull requests are welcome, in particular for the items above. Please keep to the existing constraints: **C89-style C that compiles with
ZETA's gcc 2.95**, no external libraries in the daemon, and no Intel firmware or secrets in any commit (the `.gitignore` is there for that). New
behaviour should come with a log excerpt from a real machine.

Useful reading when changing the driver: OpenBSD `sys/dev/pci/if_iwi.c` and `if_iwireg.h`, FreeBSD `sys/dev/iwi/`, Linux
`drivers/net/wireless/intel/ipw2x00/ipw2200.c`, and the first paragraphs of [04-kernel-driver.md](04-kernel-driver.md).
