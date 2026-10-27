# 14. The Wi-Fi panel

`panel/WifiPanel.cpp` is a small native app that gives the driver a face: see the network you are on, see what else is in range, pick or forget
a network, enter a password. It is plain R5-style C++ (BeAPI) and builds on ZETA with `make` in `panel/`.

```
 ┌ Wi-Fi ──────────────────────────────────────┐
 │ Connected to MyNetwork                        │   status: from /tmp/iwid.status
 │ 192.168.0.235   via 192.168.0.1   DNS ...     │
 │ Signal -52 dBm (good)   channel 11            │
 │ ┌───────────────────────────────────────────┐ │
 │ │ ▂▄▆█ MyNetwork            connected  WPA2 │ │   list: from /tmp/iwid.scan
 │ │ ▂▄▆  Neighbour-5G                    WPA2 │ │
 │ │ ▂▄   Guest                  saved    WPA2 │ │
 │ │ ▂    CafeNet                         open │ │   greyed = not supported (only WPA2/AES is)
 │ └───────────────────────────────────────────┘ │
 │ Rescan briefly interrupts the connection.     │
 │ [Rescan] [Connect...] [Forget]                │
 └───────────────────────────────────────────────┘
```

## How it works: files, not a private protocol

The panel never touches the card. Only one program may own the card, so the daemon (`iwid`) publishes its state in small text files and takes
commands through another. That keeps the panel simple and means anything (a shell script, a Deskbar replicant, `cat`) can read the state.

| File | Written by | Content |
|---|---|---|
| `/tmp/iwid.status` | `iwid` every 2 s (and on every change) | `state=connected\|scanning\|connecting\|reconnecting\|restarting\|no-network\|failed\|starting`, `error=` (a message), `ssid=`, `bssid=`, `chan=`, `dbm=`, `ip=`, `router=`, `dns=`, `networks=` (saved), `updated=` (seconds since boot) |
| `/tmp/iwid.scan` | `iwid` after every scan | one line per access point: `channel\|dBm\|security\|bssid\|ssid` (security: 0 open, 1 WEP, 2 WPA, 3 WPA2) |
| `/tmp/iwid.cmd` | the panel | one line, consumed (deleted) by the daemon within a second: `scan`, `reload`, or `connect <ssid>` |

Both output files are written to a temporary name and renamed, so a reader never sees half a file. `updated` uses the same clock as the panel
(`system_time()`), so "the daemon is alive" means "`updated` is at most 8 seconds old"; otherwise the panel shows *Wi-Fi service is not running* and a
*Start Wi-Fi service* button (which runs `start-wifi.sh`).

## The saved networks (`iwi.conf`)

The config file now holds any number of networks (up to 8): each `ssid=NAME` line followed directly by its `psk=PASSWORD` line. The daemon joins the
strongest saved network that is in range; `connect <ssid>` from the panel makes one the first to try. The panel edits the file through
`panel/wificonf.h`, which has no Be dependencies and its own tests (`panel/test_wificonf.c`, 17 cases: add, replace, forget, CRLF, spaces,
duplicates, a too-small buffer…), and it copies the previous file to `iwi.conf.bak` before every write — because that file holds your Wi-Fi
passwords and a bug there would break Wi-Fi at the next boot.

## What the buttons do

* **Rescan** → writes `scan`. Because a scan while connected crashes the card's firmware, the daemon **restarts the card, scans, and reconnects**
  (about 1.6 s offline). The window says so.
* **Connect…** → for a *new* network: a window asks for the password (hidden typing, optional "show password"), saves the network in `iwi.conf`, writes
  `connect <ssid>`. For a *saved* network: asks "Connect / Change password / Cancel". Not available for networks the driver cannot use (open, WEP, WPA/TKIP).
  Double-click on a row does the same as Connect.
* **Forget** → removes a saved network from `iwi.conf` (after a confirmation) and writes `reload`.
* **Menu → Start Wi-Fi service** — starts `start-wifi.sh` if the daemon is not running.

`connect` and `reload` make the daemon re-read the file, **restart the card**, scan and connect — so they also take about two seconds offline.

## Build and install

```sh
cd panel
make test        # the config-editing tests (any machine)
make             # → WifiPanel   (on ZETA; gcc 2.95, links only -lbe)
make install     # copies it to /boot/home/ZetaWifi/WifiPanel and links "Wi-Fi Panel" on the Desktop
```

## Limits

* One window per launch (no single-instance logic yet); closing the window quits the app — the Wi-Fi itself keeps running (the daemon is separate).
* Signal shown for the connected network is a smoothed value from received data frames, so it can be stale on an idle link.
* Passwords are stored in clear text in `iwi.conf` (see [12-security.md](12-security.md)).
* The list is the result of the last scan, which was taken when the daemon last connected or when you pressed Rescan.
