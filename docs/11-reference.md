# 11. Reference

Everything here was read from the BSD/Linux sources or confirmed on the test machine.

## PCI

| | |
|---|---|
| Vendor | 0x8086 (Intel) |
| Devices | 0x4220 (2200BG), 0x4221 (2225BG), 0x4223, 0x4224 (2915ABG) |
| BAR0 | 4 KB memory window (registers + ring registers) |
| PCI config 0x40, bits 15:8 | cleared by the driver before use (as OpenBSD) |

## Control/status registers (BAR0 offsets)

| Offset | Name | Notes |
|---|---|---|
| 0x008 | `INTR` | interrupt status; write the bits you read to acknowledge |
| 0x00c | `INTR_MASK` | left 0 (polled) |
| 0x010 / 0x014 | `INDIRECT_ADDR` / `INDIRECT_DATA` | access to NIC memory; 1/2/4-byte data accesses |
| 0x018 / 0x01c | `AUTOINC_ADDR` / `AUTOINC_DATA` | sequential NIC memory access |
| 0x020 | `RST` | `0x001 PRINCETON_RESET`, `0x080 SW_RESET`, `0x100 MASTER_DISABLED`, `0x200 STOP_MASTER` |
| 0x024 | `CTL` | `0x1 CLOCK_READY`, `0x2 ALLOW_STANDBY`, `0x4 INIT` |
| 0x030 | `IO` | `0x10000` radio enabled |
| 0x200 / 0x204 | `CMD_BASE` / `CMD_SIZE` | command ring physical address / entries |
| 0x208+8n / 0x20c+8n | `TX_BASE(n)` / `TX_SIZE(n)` | n = 0..3 |
| 0x280 | `CMD_RIDX` | card's command read index |
| 0x284+4n | `TX_RIDX(n)` | |
| 0x2a0 | `RX_RIDX` | card's RX write position |
| 0x500+4i | `RX_BASE(i)` | physical address of RX buffer i (i = 0..31) |
| 0x610 | error-log pointer | NIC-memory address of the firmware's error log (0 if none) |
| 0xf80 / 0xf84+4n / 0xfa0 | `CMD_WIDX` / `TX_WIDX(n)` / `RX_WIDX` | host write indices |
| 0xff4 | `READ_INT` | `0x20000000` INIT_HOST during reset |

Interrupt bits: `0x00000002` RX done, `0x00000800` command done, `0x00001000…0x00008000` TX1–TX4 done, **`0x01000000` FW_INITED**,
`0x04000000` RADIO_OFF, **`0x40000000` FATAL_ERROR**, `0x80000000` PARITY_ERROR.

NIC memory addresses used: `0x300040` EEPROM control, `0x300004` event control, `0x3000a0/a4/d0` firmware DMA engine,
`0x3000e0`, `0x200000…0x200010` microcode loader, `0x27000` command-block area.

EEPROM: MAC address at word 0x21..0x23 (little-endian bytes).

## Firmware commands (`IWI_IOC_CMD` type numbers)

| # | Name | Payload |
|---|---|---|
| 2 | ENABLE | none |
| 6 | SET_CONFIG | 20 bytes (below) |
| 8 | SET_ESSID | the SSID bytes |
| 11 | SET_MAC_ADDRESS | 6 bytes |
| 15 / 16 | SET_RTS_THRESHOLD / SET_FRAG_THRESHOLD | u32 |
| 17 | SET_POWER_MODE | u32 (0 = CAM) |
| 18 / 19 | SET_GROUP_KEY / SET_PAIRWISE_KEY | not used (software crypto) |
| 21 | ASSOCIATE | 40 bytes (below) |
| 22 | SET_RATES | `{mode, nrates, type, 0, rates[12]}`; mode 0=11a, 1=11b, 2=11g; type 0 = negotiated, 1 = supported |
| 25 | SET_QOS_PARAMS | not used |
| 26 | SCAN | 96 bytes (below) |
| 31 | SET_OPTIE | the IE the firmware adds to the association request |
| 33 | DISABLE | none |
| 34 | SET_RANDOM_SEED | u32 |
| 35 | SET_TX_POWER | `{nchan, mode, {chan, power}[37]}` |
| 42 | SET_SENSITIVITY | `{s16 rssi_dbm, u16 0}` |
| 84 | SET_QOS_CAP | not used |

**`SET_CONFIG` (20 bytes):** `[0] bluetooth_coexistence, [1] rsvd, [2] answer_pbreq, [3] allow_invalid_frames, [4] multicast_enabled,
[5] exclude_unicast_unencrypted, [6] disable_unicast_decryption, [7] exclude_multicast_unencrypted, [8] disable_multicast_decryption,
[9] antenna, [10] rsvd, [11] bg_autodetection, [12] rsvd, [13] enable_multicast_filtering, [14] bluetooth_threshold,
[15] silence_threshold, [16] allow_beacon_and_probe_resp, [17] **allow_mgt**, [18] report_noise, [19] rsvd`.
**Keep `[17]` = 0** (see the debugging story).

**`ASSOCIATE` (40 bytes):** `[0] channel, [1] auth (0 open, 1 shared, 3 none), [2] type (0 associate), [3] rsvd, [4..5] policy (0x01 QoS, 0x02 RSN),
[6] plen, [7] mode, [8..13] BSSID, [14..21] TSF timestamp, [22..23] capability, [24..25] listen interval, [26..27] beacon interval,
[28..33] destination, [34..39] rsvd`.

**`SCAN` (96 bytes):** `[0..3] index, [4..57] channels (first byte: band 0x00 5 GHz / 0x40 2.4 GHz | count; then channel numbers),
[58..84] type[27] (0x11 passive, 0x22 directed, 0x33 broadcast, 0x44 broadcast+directed), [85] rsvd, [86..87] rsvd,
[88..89] passive, [90..91] directed, [92..93] broadcast, [94..95] bdirected` — dwell times in ms.

## Items returned by `read()`

```
 frame (byte 0 = 9):        0..3 iwi_hdr {type, seq, flags, rsvd}   4..27 iwi_frame   28.. 802.11 frame
   iwi_frame: +0..7 rsvd, +8 channel, +9 status, +10 rate, +11 rssi, +12 agc, +13 rssi_dbm, +14 signal(2), +16 noise(2),
              +18 antenna, +19 control, +20 rsvd(2), +22 length(2)           (so channel = item[12], rssi_dbm = item[17], length = item[26..27])
 notification (byte 0 = 3): 0..3 iwi_hdr   4..15 iwi_notif {rsvd[8], type, flags, len(2)}   16.. data
```
Notification types: 10 association, 11 authentication, 12 scan channel, 13 scan complete, 15 bad link, 17 beacon (missed),
18 pairwise key, 20 calibration, 25 noise.
Authentication data: `[16] state` (9 = authenticated, 0 = deauthenticated). Association data: `[16] state` (12 = associated, 0 = deassociated),
then the association response frame.

## TX descriptor (128 bytes) and command descriptor (128 bytes)

See [04-kernel-driver.md](04-kernel-driver.md).

## EAPOL-Key frame

```
 0 version(1) 1 type(1)=3  2 length(2)   4 descriptor(1)=2   5 key info(2)   7 key length(2)   9 replay counter(8)   17 nonce(32)
 49 key IV(16)   65 RSC(8)   73 key id(8)   81 MIC(16)   97 key data length(2)   99 key data
```
Key-info bits: `0x7` version, `0x8` pairwise, `0x10/0x20` key index, `0x40` install, `0x80` ack, `0x100` MIC, `0x200` secure,
`0x400` error, `0x800` request, `0x1000` encrypted key data.
Observed: msg1 `0x008a`, msg2 `0x010a`, msg3 `0x13ca`, msg4 `0x030a`, group msg2 `0x0302`.

## Driver ↔ daemon ioctls

`IWI_IOC_FWCHUNK 20001`, `UP 20002`, `CMD 20003`, `DOWN 20004`, `NETRX 20005`, `NETTX 20006`, `STATUS 20007` (see `src/include/iwi_ioctl.h`).

## BONE ethernet ioctls

`GETADDR 9999`, `INIT 10000`, `NONBLOCK 10001`, `ADDMULTI 10002`, `REMMULTI 10003`, `SETPROMISC 10004`, `GETFRAMESIZE 10005`
(+ the three extension ioctls 10006–10008 that must *not* be implemented).

## Crypto library API (`src/daemon/wpa.h`)

`sha1_*`, `hmac_sha1`, `pbkdf2_sha1(pass, salt, saltlen, iterations, out, outlen)`, `wpa_prf(key, keylen, label, data, datalen, out, outlen)`,
`aes_set_key / aes_encrypt / aes_decrypt` (AES-128), `aes_unwrap` (RFC 3394, 128-bit KEK), `ccm_encrypt / ccm_decrypt` (M = 8, L = 2).
Plain C, 32-bit `unsigned int`, no libraries; builds the same on gcc 2.95 and on a modern Mac.

## Sizes

firmware 191,154 B (boot 6,464 / ucode 16,254 / main 168,420) · driver binary ≈ 13.8 KB · daemon ≈ 45 KB.
