# 6. The WPA2 daemon (`src/daemon/iwid.c`, `wpa.c`)

`iwid` does everything between "the card has firmware" and "ZETA has an IP address". It is one C file (~1,700 lines, mostly
comments and logging) plus the crypto library. It runs as one single-threaded loop.

```
 main
  ├─ read_config()            ssid, psk, fw, gw  from /boot/home/config/settings/iwi.conf
  ├─ card_start()             upload firmware → IWI_IOC_UP → setup_card()
  ├─ PBKDF2 → PMK             (once; 4096 rounds ≈ 40 ms on the Pentium M)
  └─ loop:  connect_once()    scan → pick AP → associate() → do_handshake()
            bridge()          DHCP → ifconfig/route/DNS → forward frames until the link or card dies
            on loss: reconnect;  if the card died: card_start() again
```

Command line: `iwid [-b] [-r] [-v] [--scan] [-t] [-e] [-i] [-h]` — see [08-install-and-usage.md](08-install-and-usage.md).

## 1. Configuration

`iwi.conf`: pairs of lines `ssid=...` / `psk=...` — each pair is one **saved network**, up to 8 (`psk` is 8–63 characters; everything after `psk=` is the
passphrase, spaces allowed), optional `fw=` and `gw=`. The passphrase is never printed to the log. A missing or empty file is fine: the daemon still starts,
scans, publishes the list, and waits for the [Wi-Fi panel](14-wifi-panel.md) to add a network. When several saved networks are in range the strongest wins,
unless the panel asked for a particular one. The key (PMK) is derived per network, once, when it is first tried.

## 2. Card setup (`setup_card`) — firmware commands, in order

| Command | Payload |
|---|---|
| `SET_MAC_ADDRESS` (11) | the EEPROM MAC |
| `SET_CONFIG` (6) | 20-byte struct: `multicast_enabled=1`, `silence_threshold=30`, `report_noise=1` |
| `SET_POWER_MODE` (17) | 0 = CAM (always awake) |
| `SET_RTS_THRESHOLD` (15), `SET_FRAG_THRESHOLD` (16) | 2346 each |
| `SET_TX_POWER` (35) ×2 | 11g then 11b: 13 channels (1–13) at power 20 |
| `SET_RATES` (22) ×2 | 11g *supported* rates `2 4 11 22 12 18 24 36 48 72 96 108` (units of 500 kbit/s), 11a: none |
| `SET_RANDOM_SEED` (34) | random 32 bits |
| `ENABLE` (2) | start the radio |

## 3. Scan

`SCAN` (26) carries a 96-byte structure: `index` (0), `channels[54]` — first byte `0x40 | 13` (2.4 GHz band flag + count)
followed by channels 1…13 — `type[27]` filled with `0x33` (broadcast probe), and the *broadcast dwell time* 40 ms.
The firmware visits each channel, sends probe requests, and passes the answers up as ordinary 802.11 frames; progress is
reported with notifications 12 (scan channel) and 13 (scan complete). A whole scan takes ≈ 0.9 s.

For every beacon (frame control `0x80`) or probe response (`0x50`) the daemon records, keyed by BSSID (address 3):
SSID (IE 0), channel (IE 3, or the card's receive channel), signal byte (**dBm = byte − 112**), capability field,
beacon interval, TSF timestamp, supported rates (IEs 1 and 50) and the **RSN IE (48)**.
Candidates for connecting are APs with *exactly* the configured SSID that carry an RSN IE, strongest first.
Several access points with the same SSID (mesh/extenders) are tried in turn.

## 4. Association (`associate`)

The firmware performs the 802.11 *authentication* (open system) and *association* handshakes itself. The daemon tells it
what to do:

1. `SET_CONFIG` again, this time `disable_unicast_decryption = 1`, `disable_multicast_decryption = 1` (**the host decrypts**),
   `bg_autodetection = 1`, `allow_mgt = 0` (**must stay 0** — see the debugging story), `report_noise = 1`.
2. `SET_ESSID` (8) — the SSID bytes.
3. `SET_RATES` (22) with type *negotiated* — the AP's rates that we also support.
4. `SET_SENSITIVITY` (42): a 4-byte structure `{s16 rssi_dbm, u16 0}` with the AP's signal in dBm (`byte − 112`).
5. `SET_OPTIE` (31): the RSN information element the firmware appends to the association request:
   `30 14 01 00 | 00 0f ac 04 (group: CCMP) | 01 00 00 0f ac 04 (1 pairwise: CCMP) | 01 00 00 0f ac 02 (1 AKM: PSK) | 00 00`.
6. `ASSOCIATE` (21): 40-byte structure: channel, auth 0 (open), type 0, policy `0x0002` (RSN), mode 2 (11g), BSSID, the AP's TSF
   timestamp (8), capability `0x0001` (ESS), listen interval 10, beacon interval, destination = BSSID.

Then the daemon waits (≤8 s) for notification **11** with state 9 (*authenticated*) and notification **10** with state 12
(*associated*). From that moment the AP expects the EAPOL handshake.

## 5. The EAPOL four-way handshake (`do_handshake`)

EAPOL frames travel as ordinary unprotected 802.11 data frames carrying LLC/SNAP `aa aa 03 00 00 00 88 8e`. Offsets below
are from the start of the EAPOL header (`version, type=3, length`), 99 bytes before the key data:
`key descriptor type @4 (=2)`, `key info @5`, `key length @7`, `replay counter @9`, `nonce @17`, `IV @49`, `RSC @65`,
`key id @73`, `MIC @81`, `key data length @97`, `key data @99`.

Key-info bits: `0x0007` descriptor version (2 = HMAC-SHA1 MIC + AES key wrap — what WPA2-CCMP uses), `0x0008` pairwise,
`0x0040` install, `0x0080` ack, `0x0100` MIC present, `0x0200` secure, `0x1000` key data is encrypted.

| # | From | Key info | What the daemon does |
|---|---|---|---|
| 1 | AP | `0x008a` | save ANonce and replay counter; generate a random SNonce |
| – | | | derive **PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) ‖ max(AA,SPA) ‖ min(ANonce,SNonce) ‖ max(…))** → KCK (16) ‖ KEK (16) ‖ TK (16) |
| 2 | us | `0x010a` | SNonce, our RSN IE as key data, MIC = HMAC-SHA1(KCK, frame with MIC zeroed)[0..16] |
| 3 | AP | `0x13ca` | **verify the MIC** (a wrong passphrase stops here — "message 3 MIC does not verify"), AES-key-unwrap the key data with KEK, parse the GTK KDE (`dd len 00 0f ac 01 keyid rsvd GTK`) |
| 4 | us | `0x030a` | acknowledge (MIC, secure), no key data |

On success TK and GTK are loaded into software AES contexts and the transmit packet number is reset. If the AP repeats
message 1, the daemon resends the same message 2. Only CCMP group keys (16 bytes) are accepted; a TKIP group key is logged and
group traffic will not work.

## 6. CCMP in software

Frame layout (data frame, To-DS, no QoS):

```
 802.11 header (24)  │ CCMP header (8) │ ciphertext = encrypt(LLC/SNAP + ethertype + payload) │ MIC (8)
 CCMP header: PN0 PN1 00 (0x20 | keyid<<6) PN2 PN3 PN4 PN5
 Nonce (13 bytes):    priority(0, or the QoS TID) ‖ A2 (transmitter address) ‖ PN (6 bytes, most significant first)
 AAD (22 or 24):      FC & 0x8fc7 (subtype/retry/power/more-data bits masked) ‖ A1 ‖ A2 ‖ A3 ‖ (SC & 0x000f) ‖ [QC & 0x000f]
 CCM parameters:      M = 8 (MIC length), L = 2
```
* **Transmit** (`send_ethernet`, `build_data_to_ds`): To-DS frame, A1 = BSSID, A2 = our MAC, A3 = destination MAC;
  PN counter starts at 1 and increments for every frame; the *firmware* is told not to touch the frame (`NO_WEP` flag).
* **Receive** (`rx_to_ethernet`): From-DS data frames only; QoS data (26-byte header) is handled; unicast uses the TK, group
  uses the GTK; the MIC is checked (a failure is logged only in verbose mode); the LLC/SNAP header must look right
  (spanning-tree and other non-IP frames are dropped); the result is `DA(A1) ‖ SA(A3) ‖ ethertype ‖ payload`.
* Frames whose source address is our own MAC are dropped (the AP reflects our broadcasts back to us).
* **Not done:** receive replay protection (PN checking) — see [12-security.md](12-security.md).

### Group key refresh

APs rotate the GTK periodically with a *group key handshake*: one EAPOL-Key from the AP (key info `0x1382`-style: ack, MIC,
secure, encrypted, *not* pairwise) sent **encrypted** with the pairwise key. `group_rekey()` verifies the MIC, unwraps the new
GTK, installs it and answers with message 2 of the group handshake (`0x0302`). A *pairwise* rekey (PTK rekey) is only logged — not
implemented.

## 7. DHCP (`dhcp_acquire`, `dhcp_renew`)

The daemon includes a small DHCP client that runs over the encrypted link directly (the BONE stack is not involved until it
has an address to configure). Why: ZETA's own `dhcp_client` is driven by the network server and a Network preferences
profile — which would mean editing persistent system configuration for every Wi-Fi network.

* DISCOVER (broadcast flag set, client-id = MAC, hostname "zeta", parameter request list subnet, router, DNS, domain, lease time),
  wait 4 s for OFFER, REQUEST (requested IP + server id), wait 4 s for ACK; up to 4 tries.
* Packets are built by hand: Ethernet + IPv4 (checksum) + UDP 68→67 (checksum 0) + BOOTP, padded to 300 bytes.
* At half the lease time it broadcasts a REQUEST for the address it holds to renew.
* Then `apply_address()`:
  `ifconfig /dev/net/iwi/0 <ip> <mask> up`, and with `-r`: `route delete default` (the boot-time default route from
  `network.conf` would otherwise make `route add` fail with "Operation already in progress"), `route add default gw <router> dev /dev/net/iwi/0`,
  and `/etc/resolv.conf` is rewritten with the DHCP DNS server.

## 8. The bridge loop (`bridge`)

```
 forever:
   item = read(/dev/misc/iwi/0)
     frame      → rx_to_ethernet() → EAPOL (0x888e)? group_rekey()  else IWI_IOC_NETRX to the stack
     notification: association/authentication "state 0" → return (reconnect)
   frame = IWI_IOC_NETTX  → send_ethernet()
   timers: DHCP renew at lease/2;  status report every 60 s;  watchdog
   nothing to do? snooze 1.5 ms
```

### Watchdog

The firmware emits a notification (noise measurement) roughly ten times per second whenever it is alive. If **no item at all**
arrives for 10 s, the daemon logs "the card has been silent for 10 s", prints the card status and — if the interrupt record
contains the fatal bit — the firmware's own error log, then returns `-2`; `main()` calls `card_start()` (firmware re-upload,
`IWI_IOC_UP`) until it succeeds and reconnects, including a new DHCP exchange.

## 9. Modes and flags

| Flag | Meaning |
|---|---|
| (none) | connect once, send an ARP probe for `gw=` and report whether the reply arrived (smoke test) |
| `-b` | bridge mode (normal operation) |
| `-r` | with `-b`: take over the default route and `/etc/resolv.conf` |
| `-v` | verbose: card status every 5 s, every notification, every bridged frame, rejected data frames, MGMT frame subtypes |
| `--scan` | scan and list access points, nothing else |
| `-t` | with `-b`: ping the router with our own packets every 10 s (isolates the Wi-Fi link from the BONE stack) |
| `-e` | with `-b`: answer ICMP echo requests ourselves (same purpose) |
| `-i` | connect, then idle 40 s printing the card status each second (found the firmware crash) |

## 10. Timing seen on the test machine

firmware upload + card start ≈ 0.1 s; scan ≈ 0.9 s; PBKDF2 ≈ 40 ms; authentication + association ≈ 0.3 s; handshake < 20 ms;
DHCP ≈ 0.3 s. About 2 seconds from starting the daemon to a working link (the boot script adds the wait for the driver and the
network server: ~30 s after power-on in total).
