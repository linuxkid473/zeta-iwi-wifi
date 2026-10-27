# 4. The kernel driver (`src/kernel/iwi.c`)

About 1,000 lines. Structure of the file, top to bottom: register constants → hardware helpers (CSR/indirect memory access,
EEPROM) → DMA allocation → firmware loaders → command ring → hardware reset → queues → control-face hooks → BONE-face
hooks → driver entry points.

## Entry points and device names

```c
int32 api_version = B_CUR_DRIVER_API_VERSION;   /* 2 */
status_t init_hardware(void);                    /* returns B_OK, touches nothing */
status_t init_driver(void);                      /* get_module(pci), scan for 8086:4220/4221/4223/4224, create semaphores */
void     uninit_driver(void);                    /* stops the card, deletes areas and semaphores */
const char** publish_devices(void);              /* { "misc/iwi/0", "net/iwi/0", NULL } */
device_hooks* find_device(const char*);          /* one hook table per name */
```

`init_driver()` does **no** hardware access beyond reading PCI config space, so a dead card cannot hang boot. The card is
touched for the first time when `iwid` calls `IWI_IOC_UP` — or earlier if the network stack asks the BONE face for its
MAC address (`ETHER_GETADDR` runs `hw_reset()`, which only resets the card and reads the EEPROM).

## Talking to the card

The card exposes a 4 KB register window (BAR0). The first 0x200 bytes are the classic CSRs; the rest holds the ring
registers. It also has 4 MB of internal memory reached through an *indirect* window:

```
 write CSR_INDIRECT_ADDR (0x10) = nic address;  read/write CSR_INDIRECT_DATA (0x14)        (1, 2 or 4 byte accesses)
 write CSR_AUTOINC_ADDR  (0x18) = nic address;  repeated write CSR_AUTOINC_DATA (0x1c)      (address auto-increments by 4)
```

`csr_read/csr_write` are direct accesses; `mem_read/mem_write/mem_write1/mem_write2/mem_read1` use the indirect window.

PCI setup (`hw_reset()`): enable memory decoding and bus mastering in the PCI command register; clear PCI config byte 0x41
(a device-specific register; OpenBSD's driver clears it before use, so this driver does too); map BAR0 with `map_physical_memory()`.

## Reset and MAC address

```
 stop_master():     INTR_MASK = 0; RST |= STOP_MASTER; wait ≤50 µs for MASTER_DISABLED; RST |= PRINCETON_RESET
 move to D0:        CTL |= INIT; READ_INT = INIT_HOST; wait ≤200 ms for CLOCK_READY
 software reset:    RST |= SW_RESET; 10 µs; CTL |= INIT
 clear NIC memory:  AUTOINC_ADDR = 0; 0xc000 × write 0 to AUTOINC_DATA        (48 K words = 192 KB)
 MAC:               3 × read_prom_word(0x21 + i)  → bytes little-endian
```

`read_prom_word()` bit-bangs the serial EEPROM through `MEM_EEPROM_CTL` (NIC address 0x300040) with chip-select, clock and
data bits — a straight transcription of OpenBSD's routine.

## Loading the firmware

The firmware file (see [05-firmware.md](05-firmware.md)) holds three images: **boot**, **ucode** (microcode) and **main**.
Sequence (`do_up()`), after the reset above:

1. **Boot image via DMA command blocks** (`load_firmware`). The image is a list of records `{dst, len, data[len]}`. It is
   copied to the contiguous staging area; for each record, the driver writes *command blocks* into NIC memory at 0x27000
   (through AUTOINC): `{ctl = 0x8cea0000|chunk, src (physical), dst, ctl^src^dst}`, one per ≤ 8191-byte chunk. A final
   zero block is the sentinel. Then `RST &= ~(MASTER_DISABLED|STOP_MASTER)`, `NIC[0x3000a4] = 0x540100` starts the engine,
   the driver polls `NIC[0x3000d0]` until it passes the sentinel, writes `NIC[0x3000a4] = 0x540c00`,
   `RST = 0` and `CTL |= ALLOW_STANDBY` to start the freshly loaded code, and waits (polling `CSR_INTR`, ≤1 s) for
   **`INTR_FW_INITED`** (0x01000000).
2. **Microcode via indirect writes** (`load_ucode`): stop bus-mastering (without the Princeton reset), pulse some
   control words (`NIC[0x3000e0]`, `NIC[EVENT_CTL]`), set `NIC[0x200000]` bytes to 0x00/0x40, then write the ucode **one
   16-bit word at a time to `NIC[0x200010]`** (the BSD sources note: *"adapter is buggy, we must set the address for each
   word"*), finish with 0x00/0x80, wait for bit 0 of `NIC[0x200000]`, read `NIC[0x200004]` seven times, write 0.
3. **`stop_master()` again — including `PRINCETON_RESET`** — see the debugging story: omit that bit and the main
   firmware never starts.
4. **Program the rings** (below).
5. **Main image** via DMA command blocks exactly like step 1, again waiting for `INTR_FW_INITED`.

`gIntrSeen` accumulates every interrupt-status bit ever acknowledged since the last `UP` (cleared at the start of
`do_up()`); it is how a fatal firmware error is noticed afterwards.

## Rings

Registers (offsets in BAR0): `CMD_BASE 0x200`, `CMD_SIZE 0x204`, `TX_BASE(n) 0x208+8n`, `TX_SIZE(n) 0x20c+8n`,
`CMD_RIDX 0x280`, `TX_RIDX(n) 0x284+4n`, `RX_RIDX 0x2a0`, `RX_BASE 0x500+4i`, `CMD_WIDX 0xf80`, `TX_WIDX(n) 0xf84+4n`,
`RX_WIDX 0xfa0`.

| Ring | Entries | Entry size | Set up by |
|---|---|---|---|
| command | 16 | 128 B | `CMD_BASE` = phys, `CMD_SIZE` = 16 |
| TX 0..3 | 64 | 128 B | `TX_BASE(n)`, `TX_SIZE(n)` = 64 (only ring 0 is used) |
| RX | 32 | 2048 B buffers | `RX_BASE + 4i` = physical address of buffer i, `RX_WIDX = 31` |

Write indices belong to the host (`CMD_WIDX`, `TX_WIDX`, `RX_WIDX`), read indices to the card.
`RX_WIDX` is "the last buffer the card may fill": after consuming slot *k* the driver writes *k* (so the card sees
slots up to *k* as free and `cur-1` as the end).

## Commands

`send_cmd(type, data, len)` fills the 128-byte descriptor at `CMD_WIDX`:

```
 +0 u8 hdr.type = 1 (command)     +2 u8 hdr.flags = 0x04 (IRQ)      +4 u8 command      +5 u8 length      +8.. data[120]
```
bumps `CMD_WIDX`, then polls until `CMD_RIDX == new WIDX` (≤1000 × 1 ms, acknowledging the interrupt register each time).
The command numbers and payload structures are in [11-reference.md](11-reference.md). `IWI_IOC_CMD` exposes this to `iwid`.

## Receiving

`read()` on the control face: acknowledge interrupts; compare `RX_RIDX` with the driver's `gRxCur`; if equal return 0 bytes;
otherwise copy slot `gRxCur`, its length derived from the contents:

```
 slot[0] == 9 (frame):         len = 4 + 24 + iwi_frame.len   (802.11 frame starts at +28; channel at +12, rssi byte at +17)
 slot[0] == 3 (notification):  len = 4 + 12 + notif.len       (notification type at +12, data at +16)
```
then advance `gRxCur`, write `RX_WIDX = gRxCur-1`. Unknown slot types are returned as 32 bytes.

## Transmitting

`write()` on the control face takes one complete 802.11 *data* frame (24-byte header, or 26 with QoS) and fills TX
descriptor *idx* in ring 0 (128 bytes, offsets from the descriptor start):

```
  0  hdr.type = 0 (data)       2  hdr.flags = 0x04 (IRQ)      12 cmd = 0x0b (TX)        14 u16 len = payload length
 17  flags = 0x20 (NO_WEP: we encrypt ourselves) | 0x80 (NEED_ACK, if unicast)           18 xflags = 0x10 if QoS
 48  the 802.11 header (24 or 26 bytes)                      88 u32 nseg = 1
 92  u32 seg_addr[0] = physical address of TX data buffer idx                           116 u16 seg_len[0] = payload length
```
The payload (everything after the header — for us: CCMP header + ciphertext + MIC) is copied into TX data buffer *idx*.
`gTxCur` advances; `TX_WIDX(0)` is written. If advancing would hit `TX_RIDX(0)` the call returns `B_BUSY` and the daemon retries.

Notes: the rate and antenna fields are left 0 (firmware chooses); TX *completion* is never processed — only the card's
read index is used to avoid overrunning the ring.

## Status and the firmware's error log (`IWI_IOC_STATUS`)

Returns a snapshot (`struct iwi_status`, 48 words): `gIntrSeen`, `INTR`, `RST`, `CTL`, `IO`, `CMD_RIDX`, `RX_RIDX`, `gRxCur`,
`TX_RIDX`, `gTxCur`, `RX_WIDX`, `INTR_MASK`, and the firmware's **error log**: the register at BAR0+0x610 holds a pointer
into NIC memory; at that address is a count followed by 7-word entries
`{desc, time, blink1, blink2, link1, link2, data}` (layout from Linux `ipw2200`). `desc` values: 1 FAIL, 2 MEMORY_UNDERFLOW,
3 OVERFLOW, 4 BAD_PARAM, 5 BAD_CHECKSUM, 6 NMI, 7 BAD_DATABASE, 8 ALLOC_FAIL, 9 DMA_UNDERRUN, 10 DMA_STATUS, 11 DINO_ERROR,
12 EEPROM_ERROR, **13 SYSASSERT**, 14 FATAL_ERROR. This is how the firmware crash described in the debugging story was
diagnosed.

## The BONE face

Hooks: `net_open/close/free/control/read/write`.

* `ETHER_GETADDR` (9999): copies the MAC (`gMacCur`, from the EEPROM). If the card was never reset yet, calls `hw_reset()` first.
* `ETHER_INIT` (10000), `ADDMULTI` (10002), `REMMULTI` (10003), `SETPROMISC` (10004): accepted and ignored — the firmware is told to pass
  multicast (`multicast_enabled`) and the daemon sees everything it needs.
* `ETHER_NONBLOCK` (10001): sets a flag; the read then uses a zero timeout and returns `B_WOULD_BLOCK`.
* `ETHER_GETFRAMESIZE` (10005): 1514.
* **Everything else returns an error** — in particular `ETHER_HASIOVECS` (10007), `ETHER_GETIFTYPE` (10008) and
  `ETHER_ADDTIMESTAMP` (10006). Do not answer them: ZETA's own eepro100 does not, and answering `HASIOVECS` made BONE split every
  frame into several `write()` calls (header, IP header, data).
* `read()`: waits on `gNetRxSem` (blocking, interruptible), pops a frame from the "to stack" queue. `close()` sets a flag and
  releases the semaphore so a blocked reader returns `B_INTERRUPTED`.
* `write()`: 14 ≤ length ≤ 1514, pushed to the "from stack" queue; if the 64-entry queue is full the frame is dropped
  (like a busy NIC).

## `iwid`-facing ioctls on the control face

| Code | Argument | Meaning |
|---|---|---|
| 20001 `IWI_IOC_FWCHUNK` | `{offset, len ≤ 4096, data}` | store part of the firmware file (allocates the blob area on first use) |
| 20002 `IWI_IOC_UP` | `{result, mac[6]}` | full reset + load all three images; `result` 0 = firmware running, negative = stage that failed |
| 20003 `IWI_IOC_CMD` | `{type, len, result, data[120]}` | send one firmware command and wait |
| 20004 `IWI_IOC_DOWN` | – | `DISABLE` command, stop bus mastering |
| 20005 `IWI_IOC_NETRX` | `{len, data[1536]}` | give a received Ethernet frame to the stack |
| 20006 `IWI_IOC_NETTX` | `{len, data[1536]}` | take the next frame the stack wants to send (`len` 0 = none) |
| 20007 `IWI_IOC_STATUS` | 48 words | register snapshot + firmware error log |

`IWI_IOC_UP` result codes: `-1` no card, `-2` no/old firmware, `-3` inconsistent firmware file, `-4` BAR0 not assigned,
`-5` register mapping failed, `-6` clock never ready, `-7` out of contiguous memory.
Boot image failed: `-30` image too large, `-31` command blocks not processed, `-32` fatal/parity error, `-33` no FW_INITED (code = -20 + r, r = -10…-13).
Microcode failed: `-31` master did not stop, `-32` microcode never answered (code = -30 + r) — these two numbers overlap with the boot-image ones.
Main image failed: `-50…-53`, same meaning as the boot-image codes (code = -40 + r).

## Unloading

`uninit_driver()` stops the card, deletes all areas (blob, staging, rings, TX buffers, registers) and the semaphores. It runs
when ZETA unloads the driver — i.e. after the last handle on either face closes. While the BONE stack holds
`/dev/net/iwi/0` open the driver stays loaded (and keeps the firmware blob); to replace the driver binary on a running
system: stop `iwid`, `ifconfig --del /dev/net/iwi/0`, copy the new file, `rescan iwi`.

## Things deliberately not done

No interrupt handler; no TX-completion processing; no multi-queue (QoS/WMM) TX; no scatter/gather writes (one segment per
frame); no power management; no support for the 2915ABG's 5 GHz channels; no PCI power-state management.
