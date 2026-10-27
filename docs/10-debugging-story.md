# 10. The debugging story

How this driver actually came about, in order, with every wrong turn that cost time. Written down because the *kinds* of bug
here (undocumented hardware, a 20-year-old OS, a closed firmware) are the kind that will bite the next person too.

The development loop: write code on a Mac, copy it to the laptop over `ssh`, compile there with its native gcc 2.95, and run
it — one test = one `ssh` session (the old `sshd` closes after every session). Wired Ethernet was the safety line.

## 0. Is it even the card I think it is? (hour 0)

"Centrino b/g" could have been an Intel 2100, 2200, a Broadcom — all very different. ZetaPC-Info showed *"Ethernet Card:
Intel – MPCI3B – Intel 2200 mPCI 3B – RoW, vendor 0x8086, device 0x4220"*. That is the PRO/Wireless 2200BG, which has open-source
drivers on OpenBSD, FreeBSD and Linux. Decision: use the BSD `iwi` driver (small, ISC-licensed) as the reference rather
than Linux's 12,000-line `ipw2200`.

## 1. Learn ZETA's driver ABI from its own driver

No ZETA driver documentation survives, but ZETA's `eepro100` is **not stripped**. Disassembly (`objdump -d` on the Mac)
of `speedo_control` showed `op - 9999` indexing a 7-entry jump table: the ioctl numbers are exactly Haiku's
`ether_driver.h` enum (see [07](07-network-integration.md)); `api_version` is 2; the imports are the BeOS R5 kernel API.
Result: Haiku's headers and driver samples are valid references.

## 2. Stage 2a — "can I even talk to the card?"

First driver (`iwiprobe`): publish a *text device*; on the first `cat /dev/misc/iwiprobe/0`, map BAR0, reset the card (OpenBSD's
sequence) and read the MAC address out of the EEPROM. Nothing touches the card at boot.

**Bug 1 — a driver that silently does not load.** The first build linked with `gcc -nostart` and ZETA's `kernel_intel`. The node
`/dev/misc/iwiprobe/0` never appeared. `nm` showed undefined symbols `__register_frame_info`, `__deregister_frame_info`,
`get_image_symbol` — pulled in by the C runtime start files, none exported by the kernel. Fix: also `-nostdlib`. (A driver
that fails to resolve a symbol is just not published — no error message anywhere.)

Result: *"card 8086:4220 rev 05 … clock: ready … MAC 00:0e:35:…"* — Intel's OUI, so register access and the EEPROM work.

## 3. Stage 2b — boot the firmware

Downloaded OpenBSD's firmware package (the Intel licence was shown to and accepted by the user), uploaded the 191,154-byte file through the driver
and implemented the three-stage load.

**Bug 2 — ZETA unloads the driver when the last handle closes.** `cat firmware > /dev/…` followed by `cat /dev/…` found no
firmware: between the two commands every handle was closed, the driver was unloaded, and its memory freed. Fix: do
*upload and use in one open* (a small C program, later `iwid` itself).

**Bug 3 — main firmware would not start.** Boot image: OK (`INTR_FW_INITED`). Microcode: OK. Main image: timeout.
OpenBSD's `iwi_stop_master()` — which is called between ucode and main image — *also sets `PRINCETON_RESET`* after stopping bus
mastering; the first version of `stop_master()` did not. One missing bit. After the fix: *"FIRMWARE RUNNING"*.

## 4. Stage 3 — commands and scanning

Command ring, `SET_CONFIG`/`ENABLE`/`SCAN`, parse beacons. First scan: 10 access points in under a second, including the
target (the screen showed it as *WPA2*, from the RSN information element in the beacon). Lesson: the frame layout
the firmware passes up is `iwi_hdr(4) + iwi_frame(24) + 802.11 frame` — an off-by-four in the offsets (`rssi_dbm` at `frame+13`,
`len` at `frame+22`, i.e. +17/+26 in the slot) was the only trap.

## 5. Stage 5 — WPA2 (the part I expected to be hardest, and wasn't)

Key finding: OpenBSD's `iwi(4)` never installs keys into the firmware — it tells the card to **not decrypt**
(`disable_unicast_decryption`, `disable_multicast_decryption`), sets the NO_WEP flag on transmit, and runs CCMP in software
in net80211. So the whole supplicant can live in userland, speaking raw 802.11 data frames through the driver. The thin-kernel
architecture followed from that.

The crypto (SHA-1, HMAC, PBKDF2, AES, AES-unwrap, CCM) was written from the standards and **verified on the Mac before it ever touched
the laptop**: IEEE 802.11i's PBKDF2 test vectors, FIPS-197, RFC 3394, RFC 2202, and CCM/PRF cross-checked against Python's
`cryptography`/`hashlib` (`tests/test_wpa.c`).

Result on the first real attempt: *association → EAPOL message 1 → 2 → 3 (MIC verified, i.e. the passphrase and every
derivation were right) → 4 → an encrypted ARP probe for the router got a decrypted ARP reply.* Link-up in 1.4 s.

## 6. The BONE device (and the first real wall)

Added `net/iwi/0`, the two frame queues, and a DHCP client in the daemon. ZETA's stack accepted the interface and showed the DHCP lease. Then
**nothing worked**: `ping` to the router, from the Mac to the Dell, everything lost.

**Bug 4 — `ETHER_HASIOVECS`.** The verbose bridge log showed the stack's frames arriving as `len 14`, `len 28`:
BONE was writing each Ethernet frame in *pieces*. I had answered the optional `ETHER_HASIOVECS`/`GETIFTYPE` ioctls that ZETA's
own driver ignores. Removing them gave whole frames (`len 42` ARP, `len 98` ICMP). Pings still failed.

**Taking the stack out of the picture.** Two debugging modes were added to the daemon that use *our own* packets only: `-t`
(send ICMP echo requests to the router) and `-e` (answer echo requests ourselves). Result: **the router answered all four
pings at +0 s and +5 s, and nothing at +18 s** — the air path was fine for a short while and then died. So not the stack.

## 7. "The link dies after 10 seconds"

The hardest bug. Timeline of what was tried:

1. *Is it the AP?* No deauthentication/disassociation frames, no notifications from the firmware.
2. *Is the card still receiving?* A per-5-second counter showed items, beacons, data and notifications going to **zero**.
   The whole receive side stopped, including the firmware's own noise notifications.
3. *Read the card.* An `IWI_IOC_STATUS` ioctl returned the interrupt-status bits ever seen: `00001802` → `40001802` — bit
   `0x40000000` is `INTR_FATAL_ERROR`. **The firmware itself was crashing.**
4. *Ask the firmware why.* The Linux `ipw2200` driver reads a crash log from card memory (pointer at register 0x610). Implementing that read
   gave `desc 13 = SYSASSERT, data 0xd0` followed by DMA-status entries — the same code addresses every time.
5. **Red herring: stale state.** An idle test seemed to show the fatal bit *before any traffic*. It was left over from the previous run: the
   driver stays loaded (the stack holds it open) and `gIntrSeen` was never cleared at start. Fixed — and a reminder that "the
   driver outlives the daemon" matters when reading state.
6. **Red herring: sensitivity.** `SET_SENSITIVITY` was being sent the raw signal byte (94) instead of a signed dBm value. FreeBSD's source
   says the byte is *dBm + 112* (`IWI_RSSI_TO_DBM`) and the command takes a signed 16-bit dBm. A real bug — now fixed (`-18 dBm` etc.) — but the
   crashes continued.
7. **The test that decided it.** `iwid -i`: associate, then *send nothing at all* and poll the status once a second. The firmware still died
   (2 s and 7 s after the handshake in two runs), so our transmissions were not the trigger. The only other difference from the BSD drivers
   was that `allow_mgt` was **1** (my copy-paste of "receive management frames so the scan works") — which makes the firmware hand up *every
   beacon of every network in range*, ~40 frames/s of noise. `iwid -i` with `allow_mgt = 0` (a temporary `-m` flag during the experiment; it is now simply the default): **40 s idle, no error.** Then a full traffic run:
   stable. It stayed stable across reboots and 5+ minute runs.

*Why exactly* the firmware asserts under that load is not known. Plausible: receive-buffer exhaustion in the firmware's internal accounting with a
polled host (no RX interrupts), since the same driver code is fine with the flood switched off. The fix is simply: **do not ask for
management frames** (`allow_mgt = 0`, with the comment in the source explaining why).

## 8. Routes, DNS, boot

* **Boot hook**: `UserBootscript` is *sourced* into ZETA's system Bootscript, so a blocking command would stall the boot. The hook is a single
  backgrounded line starting `start-wifi.sh`, which does the waiting.
* **`add_route: Operation already in progress`** when adding the default route: the boot-time default route from `/etc/network.conf` already exists.
  The daemon now does `route delete default` first.
* **Simulated boot test**: restore the Ethernet default route, kill the daemon, start `start-wifi.sh`, check the result — then a real reboot.
  Wi-Fi was up 30 s after power-on with the right route and DNS.

## 9. One drop that was never explained

After the simulated-boot test the link went silent about two minutes in (a received-frames counter stopped moving) and was not
reproducible afterwards (5+ minutes clean, a real reboot clean). Because the cause is unknown, the daemon got a **watchdog**: 10 s of silence from
the card → log registers and the firmware crash report → full card restart (firmware reload) and reconnect. It has not triggered since.
If you reproduce it, the log lines `card has been silent` + `FIRMWARE ERROR LOG` are exactly what is needed for a bug report.

## Things that looked like driver bugs but were not

* The Mac's USB Ethernet adapter disappearing (`en9` gone) — made the laptop look "dead" twice.
* The wired link showing *link up, no packets* until the network settings were re-applied.
* `ping` taking 2–4 s for the first replies: ARP warm-up on both sides.
* The Mac not seeing the laptop's Wi-Fi address right away for the same reason.

## What this approach would not have found without the status ioctl

Everything from step 7. **If you write a driver for a firmware-driven device, build a way to read the firmware's own crash report on day one.**
