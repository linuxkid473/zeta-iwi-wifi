#!/bin/sh
# install.sh - install the iwi driver and the iwid daemon on ZETA.
#
#   sh scripts/install.sh           install driver + daemon + starter script (nothing changes at boot)
#   sh scripts/install.sh --boot    additionally add ONE line to ~/config/boot/UserBootscript so Wi-Fi starts at every boot
#   sh scripts/install.sh --panel   additionally install the Wi-Fi panel (needs `make -C panel` first) and put it in the
#                                   Deskbar's  Software > Internet  menu
#   (the options can be combined)
#
# Run `make` first (it builds ./iwi and ./iwid).  Safe to run again.
# Nothing is installed system-wide: the driver goes into your user add-ons, so ZETA's boot menu option
# "disable user add-ons" removes it again if anything goes wrong.

DEST=/boot/home/ZetaWifi
K=/boot/home/config/add-ons/kernel/drivers
SETTINGS=/boot/home/config/settings
BOOT=/boot/home/config/boot/UserBootscript

[ -f iwi ] && [ -f iwid ] || { echo "run 'make' first (it builds ./iwi and ./iwid)"; exit 1; }

mkdir -p $DEST $K/bin $K/dev/misc $K/dev/net || exit 1
cp iwid $DEST/iwid
cp scripts/start-wifi.sh $DEST/start-wifi.sh
chmod +x $DEST/start-wifi.sh $DEST/iwid

cp iwi $K/bin/iwi
ln -sf ../../bin/iwi $K/dev/misc/iwi
ln -sf ../../bin/iwi $K/dev/net/iwi
rescan iwi

if [ ! -f $SETTINGS/iwi.conf ]; then
	cp config/iwi.conf.example $SETTINGS/iwi.conf
	echo "created $SETTINGS/iwi.conf  -> edit it: put your network name and password in"
fi

BOOTHOOK=no
PANEL=no
for arg in "$@"; do
	[ "$arg" = "--boot" ] && BOOTHOOK=yes
	[ "$arg" = "--panel" ] && PANEL=yes
done

if [ "$PANEL" = "yes" ]; then
	if [ -f panel/WifiPanel ]; then
		cp panel/WifiPanel $DEST/WifiPanel
		chmod +x $DEST/WifiPanel
		MENU=/boot/home/config/ZetaMenu/Software/Internet
		if [ -d $MENU ]; then
			ln -sf $DEST/WifiPanel "$MENU/Wi-Fi"
			echo "Wi-Fi panel installed: Deskbar > Software > Internet > Wi-Fi"
		else
			echo "Wi-Fi panel installed at $DEST/WifiPanel (no $MENU folder here: start it from Tracker)"
		fi
	else
		echo "panel/WifiPanel is not built: run 'make' in the panel folder first"
	fi
fi

if [ "$BOOTHOOK" = "yes" ]; then
	if grep start-wifi $BOOT > /dev/null 2>&1; then
		echo "boot hook is already in $BOOT"
	else
		[ -f $BOOT ] && cp $BOOT $BOOT.bak-before-wifi
		echo "" >> $BOOT
		echo "# Wi-Fi: iwi driver + iwid daemon (see $DEST), started in the background" >> $BOOT
		echo "$DEST/start-wifi.sh > /dev/null 2>&1 &" >> $BOOT
		echo "added the boot hook to $BOOT (backup: $BOOT.bak-before-wifi)"
	fi
fi

echo ""
echo "installed.  Still needed:"
[ -f $DEST/iwi-bss ] || echo "  1. the Intel firmware:  sh scripts/get-firmware.sh      (you must read and accept Intel's licence)"
echo "  2. your network name and password in $SETTINGS/iwi.conf"
echo "  3. try it:   $DEST/iwid --scan        then       $DEST/iwid -b -r -v"
[ "$PANEL" = "yes" ] || echo "  (optional) the Wi-Fi panel:  make -C panel && sh scripts/install.sh --panel"
