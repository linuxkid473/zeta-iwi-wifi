# 1. Overview

## The problem

ZETA 1.2.1 on a Dell Inspiron 510m had two ways to get on a network: the built-in Ethernet port, and — in theory —
Wi-Fi through ZETA's *NDIS wrapper*, which loads Windows XP `.sys` miniport drivers. On this laptop's Intel
PRO/Wireless 2200BG the NDIS wrapper panics inside `ndis_interrupt` and, if left configured, hangs the boot at
"connecting to network". And even a working NDIS driver would not help much: ZETA predates WPA2 networks being the norm
and ships no supplicant. In 2026 almost every access point is WPA2-only, so "Wi-Fi on ZETA" means writing the driver *and*
the security layer.

## The idea

Intel's card is a "soft-MAC-lite" design: **the firmware does most of the 802.11 work** — scanning, authentication,
association, rate selection, acknowledgements, retries. The host driver only has to
(a) load the firmware, (b) send it commands, and (c) shuttle 802.11 frames in and out of DMA rings.
What the firmware does *not* do (in the way the BSD drivers use it) is crypto: after association the host must run the
EAPOL handshake and encrypt/decrypt every data frame itself — OpenBSD's `iwi(4)` does this in software, which is exactly
what makes a userland supplicant feasible.

So the project is split in two:

1. **`iwi` — a thin kernel driver.** Owns the PCI device, loads the firmware, programs the rings, sends commands, hands raw
   frames to userland, and also publishes `/dev/net/iwi/0` — a BONE-style Ethernet device whose frames are carried by
   two queues. It contains no protocol logic and no crypto. Small kernel code means small crash risk — important when
   every crash is a reboot of a 20-year-old laptop.
2. **`iwid` — a userland daemon.** Everything else: scan, choose the access point, associate, WPA2 four-way handshake,
   CCMP, DHCP, routes, DNS, reconnect and a watchdog. It can be rebuilt and restarted in seconds without touching the kernel.

```
   ZETA applications (browser, ssh, ftp...)
                  │ sockets
          BONE network stack (net_server)
                  │  /dev/net/iwi/0   ← looks like an Ethernet card: whole Ethernet frames, ETHER_* ioctls
 ┌────────────────┴───────────────────────────────────────────────┐
 │ iwi (kernel)       queue "to stack"  ◄── IWI_IOC_NETRX ──┐     │
 │                    queue "from stack" ──► IWI_IOC_NETTX ─┤     │
 │                                                          │     │
 │ /dev/misc/iwi/0 ◄── ioctl: firmware, start, commands ────┤     │
 │                 ◄── read(): raw 802.11 frames/notifs ────┤ iwid│  (userland)
 │                 ──► write(): raw 802.11 data frames ─────┘     │
 │ DMA rings: command, 4 × TX, 32 × RX                            │
 └────────────────┬───────────────────────────────────────────────┘
                  │ PCI memory-mapped registers + DMA
          Intel PRO/Wireless 2200BG  (running Intel's firmware)
                  │ radio
              access point
```

## Why not ...

* **... port the Haiku / FreeBSD driver?** Haiku's `iprowifi2200` is FreeBSD's `iwi` running on a FreeBSD compatibility layer
  that needs Haiku's `net80211` stack. ZETA has neither. Porting the compatibility layer would be larger than writing the
  small driver the card needs.
* **... put the crypto in the kernel?** More kernel code = more ways to hang the machine, and kernel debugging on ZETA means
  reboots. In userland a bug is a daemon restart, and the crypto could be verified against test vectors on a Mac.
* **... use interrupts?** ZETA's interrupt API is available, but a polled design has no way to cause an interrupt storm
  and is much easier to reason about. The cost is a ~1.5 ms polling loop in the daemon (see [docs/12-security.md](12-security.md)
  and the roadmap for the trade-offs).
* **... use the NDIS wrapper?** It panics (see above) and cannot do WPA2.

## What the user sees

After boot (~30 seconds): `ifconfig` lists `/dev/net/iwi/0` with the card's MAC and an address from the router. The
default route and `/etc/resolv.conf` point at the router, so browsers, `ping`, `ssh`, `wget` work over Wi-Fi. Ethernet
keeps working too. `/tmp/iwid.log` explains what happened.

## Design principles

* **Thin, boring kernel code.** Only the hardware protocol. No allocation in the data path, no interrupt handler,
  `init_driver()` only scans PCI, so even a broken card cannot stop ZETA from booting.
* **Everything risky in userland**, restartable, loggable.
* **Evidence over guessing.** The daemon can read the card's registers and the *firmware's own crash report*; the debugging
  story shows why that mattered.
* **No dependencies.** The crypto is ~450 lines of C (SHA-1, HMAC, PBKDF2, AES-128, key unwrap, CCM) with a test suite.
* **Reversible.** Everything installs into the user add-ons; the boot hook is one line with a backup; ZETA's boot menu
  "disable user add-ons" removes the driver.

## Scope and honesty

This is a hobby project verified on one laptop. It implements the minimum for a modern home network: WPA2-PSK with CCMP.
See the status table in the [README](../README.md) and the [roadmap](13-roadmap.md) for what is missing.
