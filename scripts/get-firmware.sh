#!/bin/sh
# get-firmware.sh - fetch Intel's firmware for the PRO/Wireless 2200BG (the file "iwi-bss") and put it
# where iwid looks for it:  /boot/home/ZetaWifi/iwi-bss
#
# The firmware is Intel's and is NOT part of this project: its licence allows use only together with Intel
# hardware and forbids modification, and it must travel with the licence text.  This script shows you the licence
# and only continues if you type "yes".  It downloads the OpenBSD "iwi-firmware" package (the firmware in the
# header format the iwi driver family uses), which bundles Intel's licence file.
#
# If the URL below has moved, find "iwi-firmware" on https://firmware.openbsd.org/ and set URL=... in the environment.

URL=${URL:-https://firmware.openbsd.org/firmware/snapshots/iwi-firmware-3.1p4.tgz}
DEST=${DEST:-/boot/home/ZetaWifi}
TMP=${TMPDIR:-/tmp}/iwi-fw.$$

mkdir -p $TMP $DEST || exit 1
cd $TMP || exit 1

echo "downloading $URL"
if command -v wget > /dev/null 2>&1; then
	wget -q -O fw.tgz "$URL" || { echo "download failed"; exit 1; }
elif command -v curl > /dev/null 2>&1; then
	curl -sSfL -o fw.tgz "$URL" || { echo "download failed"; exit 1; }
else
	echo "need wget or curl"; exit 1
fi

gunzip -c fw.tgz | tar -xf - || { echo "could not unpack the download"; exit 1; }
[ -f firmware/iwi-license ] && [ -f firmware/iwi-bss ] || { echo "the package does not look right (no iwi-license / iwi-bss)"; exit 1; }

echo ""
echo "================ Intel firmware licence (read it) ================"
cat firmware/iwi-license | more
echo "=================================================================="
echo ""
echo "Do you accept this licence for your own use with your own Intel hardware? Type yes to continue:"
read answer
[ "$answer" = "yes" ] || { echo "not accepted, nothing installed"; rm -rf $TMP; exit 1; }

cp firmware/iwi-bss $DEST/iwi-bss
cp firmware/iwi-license $DEST/iwi-license.txt
echo "installed $DEST/iwi-bss  (licence kept next to it as iwi-license.txt)"
ls -l $DEST/iwi-bss
cd /; rm -rf $TMP
