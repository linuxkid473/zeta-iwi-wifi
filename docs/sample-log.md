# Sample log of a healthy run

A composite of real log lines from several runs on the test machine (`iwid -b -r`, quiet mode), put in the order they occur.
The network name and MAC addresses are masked, and the absolute timestamps (seconds since boot) are illustrative — the spacing
between lines is representative of what the test machine produced.

```
[ 23.40] firmware: 191154 bytes uploaded
[ 23.49] card is up, MAC 00:0e:35:xx:xx:xx
[ 23.78] deriving the key from your passphrase (PBKDF2, 4096 rounds)...
[ 23.82] key derived
[ 24.41] scan: 8 access points
[ 24.42] trying xx:xx:xx:xx:xx:xx on channel 11
[ 24.43] authentication notification: state 9
[ 24.43] association notification: state 12 status 29320        <- the "status" number is read from the wrong offset; state 12 = associated
[ 24.70] waiting for EAPOL message 1 from the AP
[ 24.70] message 1 received (key info 0x008a)
[ 24.70] message 2 sent
[ 24.71] message 3 received, MIC verified (key info 0x13ca)
[ 24.71] message 4 sent: handshake complete, GTK installed
[ 25.02] DHCP: discover (try 1)
[ 25.04] DHCP: offer 192.168.0.xxx from 192.168.0.1
[ 25.06] DHCP: lease 192.168.0.xxx mask 255.255.255.0 router 192.168.0.1 dns 192.168.0.1 for 86400 s
[ 25.06] running: ifconfig /dev/net/iwi/0 192.168.0.xxx 255.255.255.0 up
[ 25.12] running: route delete default
[ 25.13] running: route add default gw 192.168.0.1 dev /dev/net/iwi/0
[ 25.14] DNS server set to 192.168.0.1
[ 25.14] bridge running: /dev/net/iwi/0 is live
[ 85.14] bridge: 114 frames to the stack, 10 from it, 0 undecryptable
[ 85.14] card status (periodic): intr_seen 00001802 rst 00000000 ctl 000007c7 io 00050000 cmd_ridx 2 rx_ridx 24 rx_cur 24 tx_ridx 5 tx_cur 5
[145.14] bridge: 246 frames to the stack, 21 from it, 0 undecryptable
...
```

Reading it: the firmware is uploaded and the card started in ~0.1 s; the scan takes ~0.6–0.9 s; authentication, association and
the handshake complete in about a third of a second; DHCP adds ~0.1–0.3 s. `intr_seen 00001802` is the normal set of interrupt
bits (RX done, command done, TX done) — a `4…` in the top digit would mean the firmware crashed.
(`undecryptable` counts received data frames the daemon could not turn into Ethernet frames, e.g. the access point's spanning-tree
frames; a handful is normal.)

## Verbose excerpt (`-v`), showing the firmware's notifications

```
[..] notification type 12 flags 160 len 46 data 05 14 c2 16        <- scan: channel 5
[..] notification type 13 flags 160 len 4 data 0d 0d 01 17         <- scan complete
[..] notification type 11 flags 160 len 4 data 09 00 00 00         <- authenticated
[..] notification type 10 flags 160 len 124 data 0c 00 00 00       <- associated
[..] notification type 20 flags 160 len 104 data 00 00 00 00       <- calibration
[..] rx stats (5 s): 91 items, 14 beacons, 7 data, 68 notifications
```

## A firmware crash, as the daemon reports it (from the development logs)

```
 FIRMWARE ERROR LOG: base 00029f60, 8 entries
   #0 desc 13 time 18148165 blink 0001d27c 00005a74 link 0001b12c 0001ca30 data 000000d0      <- SYSASSERT
   #1 desc 10 time 18148169 blink 000277f0 00027290 link 01540001 00000000 data 00000000      <- DMA status (follow-up)
```
