# 12. Security notes

This is hobby software for a 2004 laptop. Read this before you trust it with anything you care about.

## What it gives you

* **WPA2-PSK with AES-CCMP**: authentication via the four-way handshake, per-session keys derived with PBKDF2 (4096 rounds) /
  PRF-384 / HMAC-SHA1, confidentiality and integrity with AES-CCM. The crypto primitives pass the standard test vectors
  (`make test`) and the handshake interoperates with a real consumer router (message 3's MIC verified, data in both directions works).

## What it does *not* give you

| Area | Status | Consequence |
|---|---|---|
| Receive replay protection | **not implemented** — the CCMP packet number of received frames is not checked | an attacker who can capture frames and re-inject them on the radio could replay old unicast/group frames; CCMP's MIC still stops *modification* |
| PTK rekey | not implemented (only logged) | an AP that insists on rekeying the pairwise key will eventually deauthenticate us; the daemon then reconnects |
| Management frame protection (802.11w) | not supported | deauthentication frames can be forged by anyone nearby (that is also true of most consumer setups) |
| WPA3, WPA (TKIP), WEP, 802.1X/EAP | not supported | |
| Randomness | `/dev/random` is used if it exists; otherwise a SHA-1 of the time and a counter | the SNonce is not predictable by a remote attacker, but the fallback is weak against a local one. Check that `/dev/random` exists on your system |
| Constant-time operations | the MIC comparison during CCM verification is branch-free; other comparisons (`memcmp` of MICs in EAPOL) are not | timing side channels are not a realistic threat here, but note it |
| Memory hygiene | keys live in static buffers and are never wiped | a crash dump or a local attacker with memory access would find them |

## Secrets on disk

* `~/config/settings/iwi.conf` contains the **passphrase in clear text**. It is created from the example with default permissions; ZETA
  has no per-user file protection to speak of. Anyone with access to the machine's disk can read it. (The passphrase is never written to the log.)
* The **PMK** (derived key) is computed at start and kept in memory only.
* `/etc/resolv.conf` is rewritten by the daemon with `-r` — it needs the daemon to run with permission to write there (it does as root on ZETA).

## The daemon runs with full privileges

On ZETA everything is effectively root. `iwid` talks to the driver through ioctls whose buffers the driver *trusts*:
`IWI_IOC_CMD` passes a user buffer directly to the firmware command path after checking only the length; `IWI_IOC_STATUS` writes
48 words to whatever pointer it is given. This is the normal trust model of BeOS-style drivers (any process that can open the device node
can already do anything), but it means: **do not expose `/dev/misc/iwi/0` to untrusted programs.**

## DHCP

The built-in DHCP client has no authentication (nor does DHCP) — a rogue DHCP server on your Wi-Fi can hand you any gateway/DNS. Same as
every DHCP client on every unsecured LAN; the WPA2 layer limits who can be on that LAN.

## Firmware

Intel's firmware is closed and runs on the card with bus-master DMA access to host memory. The driver gives it contiguous buffers
only for its rings and for firmware loading; but a malicious firmware file could attack the host over DMA (there is no IOMMU on this hardware).
Get the firmware only from a source you trust and verify what you download (`get-firmware.sh` shows the licence, not a checksum — compare the SHA-256
with a copy you trust if you care).

## Reporting problems

Open an issue on the repository. For anything security-sensitive, describe the issue without posting captures that contain your SSID, MACs or keys.
