# 9. Troubleshooting

First, look at the log: `cat /tmp/iwid.log` (or run `/boot/home/ZetaWifi/iwid -b -r -v` in a Terminal).

## Quick table

| Symptom | Likely cause | What to do |
|---|---|---|
| `cannot open /dev/misc/iwi/0` | driver not installed / not loaded | `ls /dev/misc/iwi`; re-run `scripts/install.sh`; `rescan iwi`. If the node never appears, the driver failed to load: check it was linked with `-nostdlib` (`nm iwi | grep " U "` must list only kernel functions like `acquire_sem`, `create_area`, `spin`) |
| `cannot open firmware` | no firmware file | `sh scripts/get-firmware.sh` |
| `card start failed, code -2/-3` | firmware file missing, truncated or wrong container | re-download with `get-firmware.sh` (it must be the OpenBSD `iwi-bss`, 191,154 bytes for 3.1p4) |
| `card start failed, code -6` | clock never ready | power-cycle (full shutdown) — the card is in a bad state |
| `card start failed, code -30…-33` | the **boot** image (`-30`: too large, `-31`: command blocks not processed, `-32`: fatal/parity error, `-33`: firmware never reported FW_INITED) — or, for `-31`/`-32`, the microcode stage (the ranges overlap) | power-cycle completely (shut down, don't reboot); re-download the firmware |
| `card start failed, code -50…-53` | the **main** image (same four meanings as above, +20) | usually a missing `PRINCETON_RESET` after the microcode — only if you changed the driver; otherwise power-cycle |
| scan finds nothing | card has no firmware running, or antenna/rf-kill | the Wi-Fi hardware switch / `Fn`+`F2` key on laptops that have one; `iwid --scan` |
| `no access point called '…' with WPA2 found` | wrong SSID (case!), 5 GHz-only network, or WPA/TKIP-only | check `iwid --scan`; the card only does 2.4 GHz channels 1–13 |
| `message 3 MIC does not verify (wrong passphrase?)` | wrong passphrase | fix `psk=` |
| `no EAPOL message 1 received` | association succeeded but AP never started the handshake | move closer; try again; some routers need "WPA2-PSK [AES]" only |
| `group key is N bytes (TKIP?)` | router uses TKIP for group traffic (mixed mode) | set the router to WPA2/AES only |
| `DHCP: no lease obtained` | DHCP server slow/unreachable, MAC filter | check MAC filtering on the router; look for the MAC in the router's client list |
| `ifconfig` shows `00:00:00:00:00:00` for `iwi/0` | interface only looked at, never configured | normal until `iwid` configures it |
| Wi-Fi works, internet doesn't | default route / DNS not set | run with `-r`; check `route` and `cat /etc/resolv.conf`; `add_route: Operation already in progress` means an old default route exists (`route delete default` first) |
| `ping` prints `sendto` and nothing else | no route / host unreachable (the stack cannot ARP) | `route`; check the link really is up (`tail /tmp/iwid.log`) |
| connection works for a while, then all traffic stops | firmware crashed (or AP kicked us) | the daemon's watchdog reports it after 10 s and restarts the card; read the `FIRMWARE ERROR LOG` lines |
| `the card has been silent for 10 s: restarting it` repeatedly | firmware keeps crashing | see "firmware crashes" below |
| Boot hangs / no network after reboot | user add-on problem | boot menu → *disable user add-ons* |

## Reading the status lines

```
 card status (periodic): intr_seen 00001802 rst 00000000 ctl 000007c7 io 00050000 cmd_ridx 2 rx_ridx 24 rx_cur 24 tx_ridx 5 tx_cur 5
```
* `intr_seen` — OR of all interrupt-status bits acknowledged since the last start. `0x1802` = RX done, command done, TX1 done
  (normal). **`0x40000000` (FATAL_ERROR) or `0x80000000` (PARITY_ERROR)** = the firmware died.
* `io 00050000` — bit `0x10000` is the radio-enabled bit; if it is clear, the hardware radio switch is off.
* `rx_ridx` (card's write position) should keep moving; `rx_cur` (driver's read position) should follow it closely.
* `tx_ridx == tx_cur` means the card has consumed everything we queued.

## Firmware crashes

If `intr_seen` has `0x40000000`, the daemon prints the firmware's own crash log:

```
 FIRMWARE ERROR LOG: base 00029f60, 8 entries
   #0 desc 13 time ... blink 0001d27c 00005a74 link ... data 000000d0
   #1 desc 10 time ... ... (DMA status)
```
`desc` 13 = SYSASSERT (the firmware tripped an internal assertion), 10 = DMA_STATUS (follow-up noise), 14 = FATAL_ERROR; the other codes
are in [04](04-kernel-driver.md). The `blink` (branch-link) values identify *where* in the firmware it died: the same pair
(`0001d27c 00005a74`) on every crash during development was the signature of the "too many management frames" crash
(`allow_mgt = 1`). If you see the same signature, something has turned that setting back on.

## Useful commands

```sh
cat /tmp/iwid.log | tail -30          # what the daemon says
ifconfig                              # interfaces and counters
route                                 # routing table (default should be via /dev/net/iwi/0 with -r)
cat /etc/resolv.conf                  # DNS
ps | grep -E "iwid|start-wifi"        # is it running? (note the thread id on the line below the team line)
/boot/home/ZetaWifi/iwid --scan       # needs the daemon to be stopped first (the card has one owner)
/boot/home/ZetaWifi/ethtest           # (make ethtest) asks /dev/net/iwi/0 for its MAC and frame size
```

## Stopping the daemon by hand

`ps` lists a team line (`./iwid -b -r (team N)`) followed by its thread line; ZETA's `kill` wants the **thread** id:
`/bin/kill -9 <thread id>`. If `start-wifi.sh` is running it restarts the daemon after 15 s — kill the `start-wifi.sh` thread first.

## Network flakiness on the *wired* side during development

Unrelated to the driver but worth knowing if you debug over Ethernet: on the test machine the wired link sometimes showed
"link up, zero packets received" until the network settings were re-applied, and a USB Ethernet adapter on the Mac side
occasionally dropped off the bus (the Mac's `en9` vanished) which looked exactly like "the Dell died".
Check both ends before blaming the driver.
