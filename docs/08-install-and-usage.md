# 8. Install and usage

## Requirements

* ZETA (tested: 1.2.1) with the developer tools: `gcc` (the 2.95-family compiler that comes with ZETA), `make`, headers in
  `/boot/develop/headers/be` (the driver includes `KernelExport.h`, `Drivers.h`, `PCI.h`).
* An Intel PRO/Wireless 2200BG (PCI 8086:4220), see [02](02-hardware-and-environment.md).
* A **WPA2-PSK network using AES/CCMP** (the default of virtually every modern router; "WPA2 Personal"). Mixed WPA/WPA2 networks work only if
  the *group* cipher is also AES — set the router to "WPA2 only / AES" if the log says the group key is TKIP.
* A way to get the files onto the machine (USB stick, `ftp`, `wget`, `scp`).

## 1. Build

```sh
cd zeta-iwi-wifi
make test        # crypto self-test (optional, 1 second)
make             # → ./iwi  (kernel driver)   and   ./iwid  (daemon)
```
Expected: no errors; warnings about unused variables are harmless. The driver link line deliberately uses `-nostart -nostdlib`
— see the debugging story for why a normal link produces a driver that silently refuses to load.

## 2. Install

```sh
sh scripts/install.sh
```
This copies `iwi` into `~/config/add-ons/kernel/drivers/bin/`, creates the two symlinks in `dev/misc/` and `dev/net/`, runs
`rescan iwi`, copies `iwid` and `start-wifi.sh` to `/boot/home/ZetaWifi/`, and creates `~/config/settings/iwi.conf` from the
example if there is none. **Nothing changes at boot yet.**

Check: `ls -l /dev/misc/iwi/0 /dev/net/iwi/0` — both should exist.

## 3. Firmware

```sh
sh scripts/get-firmware.sh
```
Read Intel's licence, type `yes`. Result: `/boot/home/ZetaWifi/iwi-bss`. (Background: [05](05-firmware.md).)

## 4. Configure

Edit `/boot/home/config/settings/iwi.conf`:

```
ssid=MyNetwork
psk=my password
```
The file contains your Wi-Fi password in plain text — keep it private (it is outside the repository; `.gitignore` also excludes
any `iwi.conf`).

## 5. Try it

```sh
cd /boot/home/ZetaWifi
./iwid --scan          # lists access points: channel, dBm, BSSID, SSID, [RSN]
./iwid                 # connect once, ARP-probe the router, print the result, exit
./iwid -b -r -v        # real operation, verbose: leave it running in a Terminal; Ctrl-C stops it
```
In a second Terminal while `-b` runs:

```sh
ifconfig               # /dev/net/iwi/0 should show your MAC and an address from the router
ping -c 3 192.168.0.1  # your router (adjust)
ping -c 3 1.1.1.1      # the internet by number    (needs -r, or a route you added yourself)
ping -c 3 example.com  # the internet by name      (needs -r, or DNS configured yourself)
```

The very first pings after a (re)connect can be lost while the router and the stack learn each other's addresses; try again.

## 6. Start at every boot

```sh
sh scripts/install.sh --boot
```
This backs up `~/config/boot/UserBootscript` to `UserBootscript.bak-before-wifi` and appends

```sh
/boot/home/ZetaWifi/start-wifi.sh > /dev/null 2>&1 &
```
`start-wifi.sh` waits for the driver and the network server, then runs `iwid -b -r` in a loop that restarts it 15 seconds after
it exits. Wi-Fi is up about 30 seconds after power-on. To keep Ethernet as the primary connection, remove `-r` from
`start-wifi.sh`.

## Logs

`/tmp/iwid.log` (cleared at boot). A healthy run is shown in [sample-log.md](sample-log.md). Every 60 seconds it prints
`bridge: N frames to the stack, M from it, K undecryptable` and a `card status` line. Run `./iwid -b -r -v` by hand for much more.

## Uninstall

```sh
# remove the boot hook: delete the last two lines of ~/config/boot/UserBootscript (or copy the .bak-before-wifi file back)
rm /boot/home/config/add-ons/kernel/drivers/bin/iwi
rm /boot/home/config/add-ons/kernel/drivers/dev/misc/iwi /boot/home/config/add-ons/kernel/drivers/dev/net/iwi
rm -r /boot/home/ZetaWifi          # also removes the firmware copy
```
Emergency (the machine misbehaves during boot): hold the key for the boot menu and choose **disable user add-ons** — the driver is
simply not loaded. Then remove the boot hook line.

## Replacing a running driver

The network stack keeps `/dev/net/iwi/0` open, so a loaded driver cannot be overwritten in place:

```sh
# kill iwid (and start-wifi.sh):  ps  → find the thread ids → /bin/kill -9 <thread id>
ifconfig --del /dev/net/iwi/0
cp iwi /boot/home/config/add-ons/kernel/drivers/bin/iwi
rescan iwi
```

## Working remotely on the machine

The development of this project was done over ZETA's `sshd` — start it with `sshd` in a Terminal (it is not started at boot).
It is OpenSSH 3.9, so modern clients need
`-o KexAlgorithms=+diffie-hellman-group14-sha1 -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa` and RSA keys.
Keep a wired connection as a back door while you work on a Wi-Fi driver — you cannot debug it over itself.
