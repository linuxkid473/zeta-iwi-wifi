# 2. Hardware and environment

## The test machine

| | |
|---|---|
| Laptop | Dell Inspiron 510m (2004), Intel 855GM chipset, Pentium M |
| Wi-Fi card | Intel PRO/Wireless 2200BG, mini-PCI "Type 3B", region "RoW" — PCI **8086:4220**, revision 05 |
| PCI location | bus 1, device 3, function 0, interrupt line 11 |
| Register window | BAR0, 4 KB of memory space (`0xfcffe000` on this machine) |
| Wired NIC | Intel 82801DB PRO/100 VE (8086:103D) — handled by ZETA's own `eepro100` driver |
| OS | BeOS ZETA 1.2.1 (kernel 6.2, build 20392) |
| Toolchain on the machine | gcc "2.9-beos-000224" (a 2.95-family compiler), `make`, ZETA developer headers in `/boot/develop/headers/be` |

All code in this repository was compiled **on the laptop itself** with that compiler; the code is deliberately plain
C89-style (declarations at the top of blocks, no `//` surprises, no 64-bit tricks).

## Identifying your card

Any of these shows the PCI id:

* ZETA: **ZetaPC-Info** → *Devices* → look for "Ethernet Card ... Intel 2200 mPCI 3B" with *Vendor 0x8086, Device 0x4220*
  (that is the screen this project started from).
* ZETA terminal: `listdev`.
* Windows: Device Manager → Network adapters → Details → Hardware Ids (`VEN_8086&DEV_4220`).
* Linux: `lspci -nn | grep -i 'network'`.

The driver matches these ids (all Intel, vendor 0x8086):

| Device id | Card | Status |
|---|---|---|
| **0x4220** | PRO/Wireless 2200BG | **tested** |
| 0x4221 | PRO/Wireless 2225BG | matched, untested |
| 0x4223 / 0x4224 | PRO/Wireless 2915ABG (adds 802.11a) | matched, untested; only 2.4 GHz channels are scanned/used |

If your card shows a different id (for example a Broadcom 43xx, Atheros, or the *3945/4965* "Intel Wireless WiFi Link"
cards), **this driver is not for it**. Intel's *2100* (ipw2100, 8086:1043) is a different, simpler design and is also
not supported.

## ZETA specifics that shaped the design

* **Driver API = BeOS R5 kernel driver API, api_version 2.** The disassembly of ZETA's own
  `/boot/beos/system/add-ons/kernel/drivers/bin/eepro100` (not stripped!) showed the same exports as Haiku's and
  BeOS R5's driver model: `init_hardware`, `init_driver`, `uninit_driver`, `publish_devices`, `find_device`,
  `api_version = 2`, and imports like `get_module("bus_managers/pci/v1")`, `map_physical_memory`, `create_area`,
  `get_memory_map`, `spin`. So Haiku's headers and R5 driver documentation apply.
* **BONE ethernet ioctls** are `B_DEVICE_OP_CODES_END` (= 9999) + 0…6 (GETADDR, INIT, NONBLOCK, ADDMULTI, REMMULTI,
  SETPROMISC, GETFRAMESIZE). ZETA's header `ether_driver.h` adds `ETHER_ADDTIMESTAMP`, `ETHER_HASIOVECS`,
  `ETHER_GETIFTYPE`; the eepro100 driver answers none of those three, and **neither should you** (see the debugging story).
* **A driver is unloaded when its last open handle closes.** Anything the driver was holding in memory (such as the
  uploaded firmware) is lost then. The BONE stack keeps `/dev/net/iwi/0` open once it has seen the interface, which keeps
  the driver loaded afterwards.
* **User drivers** live in `/boot/home/config/add-ons/kernel/drivers/` (`bin/<driver>` plus a symlink in `dev/<class>/`
  that tells devfs under which directory the published names appear). Holding the left shift/space at boot (boot menu)
  and choosing *disable user add-ons* removes them from the picture — your safety net.
* **Networking** is BONE: interfaces are named by their device path (`/dev/net/eepro100/0`, `/dev/net/iwi/0`), configured
  with `ifconfig` and `route`, and listed in `/etc/network.conf`. `/etc/resolv.conf` holds DNS servers.
* **`/tmp` is cleared at boot**, which is where `iwid` logs.
* There is **no `uptime`**, `kill` wants thread ids (use `/bin/kill -9 <thread id from ps>`), and the user boot script
  (`~/config/boot/UserBootscript`) is **sourced** into the system Bootscript — so anything started from it must be backgrounded.

## What the old NDIS path did

ZETA's `ndis` driver (a wrapper for Windows XP miniport drivers) with a Centrino card panics in `ndis_interrupt`, and with
the wireless interface configured the boot stops at "connecting to network". The workaround that was used to recover the
machine: boot another OS and move the symlink `…/kernel/drivers/dev/net/ndis` out of the way. That wrapper is *not* used
by this project; if you have it configured, disable it first.

## Other things the machine needed

* A way in for development: ZETA's `sshd` (OpenSSH 3.9p1 — it needs legacy algorithms:
  `-o KexAlgorithms=+diffie-hellman-group14-sha1 -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa`, RSA keys only).
  `sshd` is not started at boot; type `sshd` in a Terminal.
* The Ethernet link to the development Mac (shared via Internet Sharing) was the debugging channel while the Wi-Fi was being
  built — essential, because a broken Wi-Fi driver can't be debugged over Wi-Fi.
