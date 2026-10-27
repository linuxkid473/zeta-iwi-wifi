# 5. The Intel firmware

## Why it is not in this repository

The PRO/Wireless 2200BG is useless without Intel's firmware, a ~190 KB file the host uploads into the card at every start.
Intel's *"IPW2200 Firmware License Agreement"* (shipped with the file) says, in short:

* the software is licensed **only for use with Intel component products**;
* you may redistribute it **only together with the licence text**, and only to recipients who agree to be bound by terms at
  least as restrictive;
* **no modification**.

A public source repository cannot meet "recipient agrees to be bound", so the firmware is excluded (`.gitignore` blocks it),
and `scripts/get-firmware.sh` makes *you* read and accept the licence before the file is placed on your machine.
This is not legal advice; read the licence text yourself.

## Getting it

```sh
sh scripts/get-firmware.sh
```
It downloads OpenBSD's `iwi-firmware` package (currently `iwi-firmware-3.1p4.tgz` from
`https://firmware.openbsd.org/firmware/snapshots/`), unpacks `firmware/iwi-license` and `firmware/iwi-bss`, shows the licence
through `more`, and installs the firmware as `/boot/home/ZetaWifi/iwi-bss` (and the licence next to it) only if you answer `yes`.
If the URL has moved, find `iwi-firmware` on <https://firmware.openbsd.org/> and run `URL=... sh scripts/get-firmware.sh`.

Other places the same Intel files exist: Intel's old *ipw2200* download page (click-through licence), Debian's non-free
`firmware-ipw2x00` package (older releases) — but those are packaged in Linux's own container (`ipw2200-bss.fw`); I have not checked them
byte for byte against this driver, which expects the **BSD container** described below. Use the OpenBSD package.

Files in the OpenBSD package: `iwi-bss` (BSS / infrastructure mode — the one used here), `iwi-ibss` (ad-hoc), `iwi-monitor`
(monitor mode), `iwi-license`.

## The container format (`iwi-bss`, 191,154 bytes)

```
 offset  size  field
   0      1    oldvermaj    1
   1      1    oldvermin    0
   2      1    vermaj       3          ← the driver requires ≥ 3
   3      1    vermin       0
   4      4    bootsz       6464       (little endian)
   8      4    ucodesz      16254
  12      4    mainsz       168420
  16      ...  boot image   (bootsz bytes)
  16+bootsz    ucode image  (ucodesz bytes)
  16+bootsz+ucodesz   main image (mainsz bytes)
```
(header 16 bytes + 6464 + 16254 + 168420 = 191,154). The numbers above are for `iwi-firmware-3.1p4`; the header reports
firmware version 3.0.

* The **boot** and **main** images are lists of records `{u32 destination, u32 length, bytes[length]}` that the driver turns
  into DMA command blocks (see [04-kernel-driver.md](04-kernel-driver.md)).
* The **ucode** is a flat array of 16-bit words written one at a time into the card's microcontroller memory.

## How the driver validates and loads it

`iwid` reads the file and uploads it with `IWI_IOC_FWCHUNK` in 4 KB chunks (the kernel cannot read files, and ZETA unloads a
driver when its last handle closes, so the firmware has to be re-sent whenever the driver is freshly loaded — `iwid` simply
re-sends it at every start).

`IWI_IOC_UP` then checks: file at least 16 bytes, `vermaj ≥ 3`, none of the three sizes is zero, and the file is at least
`16 + bootsz + ucodesz + mainsz` bytes long. Failing any of these returns `-2`/`-3`.

## Behaviour worth knowing

* The firmware **asserts** (crashes) if it is given too much unwanted input — see the `allow_mgt` story in
  [10-debugging-story.md](10-debugging-story.md). Its crash report is readable (see `IWI_IOC_STATUS`).
* It reports `INTR_FW_INITED` (0x01000000) twice during start-up (after the boot image and after the main image).
* It sends *notifications* continuously while running (noise measurements about 10 per second); the daemon's watchdog uses
  that as a heartbeat.
