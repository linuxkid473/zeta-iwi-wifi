# 7. Integration with ZETA's network stack (BONE)

## What BONE expects from an Ethernet driver

Taken from the disassembly of ZETA's own `eepro100` driver (see [02](02-hardware-and-environment.md)) and ZETA's `ether_driver.h`:

* A device published as `net/<driver>/<n>` → `/dev/net/<driver>/<n>`; the **interface name is the device path**
  (`/dev/net/eepro100/0`), which is what `ifconfig` and `route` use.
* `ioctl` numbers starting at `B_DEVICE_OP_CODES_END` (9999):

  | Op | Name | eepro100 behaviour | `iwi` |
  |---|---|---|---|
  | 9999 | `ETHER_GETADDR` | copies the 6-byte MAC | same |
  | 10000 | `ETHER_INIT` | no-op | no-op |
  | 10001 | `ETHER_NONBLOCK` | sets/clears `O_NONBLOCK` for reads | same |
  | 10002 / 10003 | `ADDMULTI` / `REMMULTI` | multicast filter | accepted, ignored |
  | 10004 | `ETHER_SETPROMISC` | promiscuous mode | accepted, ignored |
  | 10005 | `ETHER_GETFRAMESIZE` | writes 1514 | same |
  | 10006 | `ETHER_ADDTIMESTAMP` | **error** | **error** |
  | 10007 | `ETHER_HASIOVECS` | **error** | **error** |
  | 10008 | `ETHER_GETIFTYPE` | **error** | **error** |

* `read()` returns one complete frame (blocking); `write()` takes one complete frame. If the driver does not provide `readv`/`writev`
  hooks (and `iwi` does not), BONE uses plain `read`/`write`.

### The `HASIOVECS` trap

My first version answered `ETHER_HASIOVECS` (10007) with `false` and `ETHER_GETIFTYPE` with `IFT_ETHER`. BONE then sent
every frame as several `write()` calls — a 14-byte Ethernet header, then the IP header, then the data — which the bridge
duly forwarded as separate, broken "frames" (the log showed `len 14`, `len 20`, `len 64`). The probable cause is that the
answer is written into a buffer the stack reads as an `int`/`bool` of unknown width. Returning an error, as `eepro100` does,
fixes it. **Do not implement the three extension ioctls.**

## What the stack does with a new device

* Devices appear in `/dev/net/` when the driver is loaded (user add-ons with the right symlink — see below).
* The stack creates an interface entry lazily, the first time something refers to it. `ifconfig /dev/net/iwi/0` on an
  interface that has never been configured shows `HWaddr 00:00:00:00:00:00` (even though the driver answers `ETHER_GETADDR`
  correctly — `tests/ethtest.c` proves it). Deleting and re-listing it did not change that. As soon as the interface is
  configured (`ifconfig /dev/net/iwi/0 <ip> <mask> up`, which is what `iwid` does after DHCP) it shows the real MAC. `iwi` answers
  `GETADDR` at any time, running a minimal hardware reset if it has to read the EEPROM first.
* While the stack holds the device open, the driver stays loaded.
* The interface has **no link-state concept**: it looks "up" whether or not the Wi-Fi is connected. Frames written while there is
  no link wait in the 64-entry "from stack" queue (and are dropped when it is full) until `iwid` connects.

## Addresses, routes, DNS

`iwid` configures the interface itself once DHCP succeeds:

```
 ifconfig /dev/net/iwi/0 192.168.0.235 255.255.255.0 up
 route delete default                                    (-r only)
 route add default gw 192.168.0.1 dev /dev/net/iwi/0     (-r only)
 /etc/resolv.conf  ←  nameserver <dhcp dns>              (-r only)
```
Without `-r` only the address is set, so an Ethernet connection keeps its routes and DNS. With `-r` Wi-Fi becomes the primary
connection. The Ethernet interface keeps its own subnet route either way, so a machine on the Ethernet subnet stays reachable.

`/etc/network.conf` (which lists interfaces for boot-time configuration) is **not modified**. The Wi-Fi interface is created and
configured by the daemon at every boot instead. That avoids ZETA's boot-time DHCP profile machinery and means uninstalling is just
removing one line from the boot script.

## Why the daemon does its own DHCP

ZETA's `dhcp_client` is not meant to be run by hand: it is an application driven by the network server, configured from a
Network preferences profile. To use it you would persistently edit the network configuration for each Wi-Fi network. A 150-line
DHCP client inside the daemon sidesteps that and works the same on every boot, on any network. The trade-off: the lease is
not visible to ZETA's Network preferences.

## Installing the driver files

```
 /boot/home/config/add-ons/kernel/drivers/bin/iwi                       the driver binary
 /boot/home/config/add-ons/kernel/drivers/dev/misc/iwi -> ../../bin/iwi    control face is published under  misc/
 /boot/home/config/add-ons/kernel/drivers/dev/net/iwi  -> ../../bin/iwi    BONE face is published under     net/
```
Both symlinks are needed: the directory a symlink sits in is the class under which devfs scans the names the driver publishes.
`rescan iwi` (or a reboot) makes ZETA look at the driver again.

## MTU and fragmentation

MTU 1500 (frame size 1514). The 802.11 header + CCMP header + MIC add 24 + 8 + 8 + 8 (LLC/SNAP) bytes on the air; the
card's firmware fragments or retries as needed (fragmentation threshold 2346 = off).

## Coexistence with the Ethernet card

Both interfaces can be up. With `-r` the default route is Wi-Fi; to prefer Ethernet instead, leave `-r` off in
`scripts/start-wifi.sh` (the boot script) — the address is still configured, only the route and DNS are untouched.
