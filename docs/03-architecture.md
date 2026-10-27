# 3. Architecture

## Components

| Component | Where it runs | Source | Job |
|---|---|---|---|
| `iwi` | ZETA kernel (user add-on) | `src/kernel/iwi.c` | PCI device, register access, firmware boot, rings, command interface, raw frame I/O, BONE ethernet face |
| `iwid` | userland daemon | `src/daemon/iwid.c`, `wpa.c` | scan, join, WPA2, CCMP, DHCP, bridging, watchdog, reconnect |
| `start-wifi.sh` | userland, from the boot script | `scripts/start-wifi.sh` | waits for driver and network server, runs `iwid -b -r` in a restart loop |
| Intel firmware | runs *on the card* | not included | 802.11 MAC: scan, authenticate, associate, retry, rates |

## The two faces of the driver

`publish_devices()` publishes two names:

| Device | Used by | Interface |
|---|---|---|
| `/dev/misc/iwi/0` — **control face** | `iwid` only | ioctls `IWI_IOC_FWCHUNK / UP / CMD / DOWN / STATUS / NETRX / NETTX`; `read()` returns one raw item from the card; `write()` sends one raw 802.11 data frame |
| `/dev/net/iwi/0` — **BONE face** | the network stack | standard ethernet driver contract: `ETHER_GETADDR`, `NONBLOCK`, `GETFRAMESIZE`..., `read()` = next received Ethernet frame (blocking), `write()` = send one Ethernet frame |

Both faces share one device state, protected by one semaphore (`gLock`).

## Data flow

### Receive (air → application)

```
 access point ─ 802.11 frame (CCMP encrypted) ─►  card + firmware
   card DMAs it into RX buffer slot N of the 32-slot ring (4-byte iwi header + 24-byte iwi_frame + 802.11 frame)
   iwid:   read(/dev/misc/iwi/0)  → the driver copies slot N out, advances its read index, tells the card the slot is free (RX_WIDX)
   iwid:   rx_to_ethernet(): CCMP decrypt (key = PTK for unicast, GTK for group), check MIC, strip LLC/SNAP → Ethernet frame
   iwid:   ioctl(IWI_IOC_NETRX)  → driver puts the frame in the "to stack" queue and releases a semaphore
   BONE:   read(/dev/net/iwi/0)  (was blocked on the semaphore) → gets the frame
```

### Transmit (application → air)

```
 BONE:   write(/dev/net/iwi/0, ethernet frame) → driver puts it in the "from stack" queue
 iwid:   ioctl(IWI_IOC_NETTX) polled every ~1.5 ms → gets the frame
 iwid:   send_ethernet(): LLC/SNAP header, 802.11 data header (To-DS), CCMP header + encrypt + MIC → one raw frame
 iwid:   write(/dev/misc/iwi/0, raw frame) → driver fills TX descriptor, 802.11 header into the descriptor, payload via DMA segment
         → writes the TX write index → card transmits (firmware adds ACK handling, retries, rate)
```

### Control

`iwid` sends firmware commands with `IWI_IOC_CMD`: the driver puts a 128-byte descriptor in the command ring, bumps
`CMD_WIDX`, and waits (polling `CMD_RIDX`, up to ~1 s) until the card has consumed it. Notifications from the firmware
(scan progress, "authenticated", "associated", noise reports, ...) arrive in the same RX ring as frames and are returned by
the same `read()`.

## Memory

All DMA memory is allocated with `create_area(..., B_CONTIGUOUS, ...)` and its physical address taken with
`get_memory_map()`; physical addresses are what the card is given.

| Area | Size | Contents |
|---|---|---|
| firmware blob | 192 KB (`B_FULL_LOCK`, not contiguous) | the firmware file as uploaded by `iwid` in 4 KB chunks |
| firmware staging | 192 KB contiguous | one firmware image at a time is copied here; the card DMAs it into itself from here |
| rings | 0x24000 contiguous | command ring (16 × 128 B at +0), four TX descriptor rings (64 × 128 B at +0x1000 + n·0x4000), 32 RX buffers (2048 B at +0x11000) |
| TX data buffers | 64 × 2048 B contiguous | payload of each in-flight TX frame (descriptor slot *i* uses buffer *i*) |
| register window | 4 KB | `map_physical_memory()` of BAR0 |
| two frame queues | 64 × 1536 B each, static | Ethernet frames between BONE and `iwid` |

## Concurrency

* One semaphore, `gLock`, serialises every entry point that touches card state (`read`, `write`, `control`).
* The BONE read blocks on a *separate* semaphore, `gNetRxSem`, which counts frames in the "to stack" queue. It is waited with
  `B_CAN_INTERRUPT`; `close` releases it so a blocked reader is woken when the stack closes the device.
* There is **no interrupt handler**. Polling means no re-entrancy and no IRQ storms (IRQ 11 is shared on this machine).
  The interrupt *status* register is still acknowledged (`ack_intr()`) on every `read`/`write`/command — the firmware
  seems to want that.
* `iwid` is a single-threaded loop: read an item from the card, deliver it, poll the stack's transmit queue, run timers
  (DHCP renew, watchdog, periodic status), sleep 1.5 ms if nothing happened.

## Boot flow

```
 ZETA boot
   kernel loads user drivers: iwi.init_driver() scans PCI, publishes misc/iwi/0 and net/iwi/0   (touches nothing)
   net_server starts, creates interfaces from /etc/network.conf (eepro100 + loop0)
   Bootscript sources ~/config/boot/UserBootscript → starts start-wifi.sh in the background
      start-wifi.sh: wait for /dev/misc/iwi/0 (≤2 min), wait until `ifconfig` answers (≤1 min)
      iwid -b -r:  read iwi.conf; upload firmware; IWI_IOC_UP; configure card; scan (~1 s)
                   join the strongest AP with the configured SSID; 4-way handshake; DHCP
                   ifconfig /dev/net/iwi/0 <ip> <mask> up;  route delete default;  route add default gw <router> dev /dev/net/iwi/0
                   write /etc/resolv.conf;  enter the bridge loop
      if iwid exits, start-wifi.sh waits 15 s and starts it again
```
Observed: Wi-Fi is up about 30 s after power-on.

## Error handling philosophy

* A failing kernel call returns an error to userland; the daemon logs it and retries or restarts the card.
* Any silence from the card longer than 10 s (the firmware normally emits a notification many times a second) is treated as
  a firmware crash: the daemon logs the card registers and the firmware's own error report, and does a **full restart**
  (firmware re-upload, `IWI_IOC_UP`, reconnect, DHCP).
* If the access point deauthenticates or deassociates us, the daemon reconnects (re-scan, re-associate, new handshake, new DHCP).
