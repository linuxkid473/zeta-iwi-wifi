#!/bin/sh
# start-wifi.sh - brings up Wi-Fi at boot (started in the background from ~/config/boot/UserBootscript).
# Waits for the Wi-Fi card driver and for the network server, then runs the iwid daemon and restarts it if it ever exits.
WIFI_DIR=/boot/home/ZetaWifi
LOG=/tmp/iwid.log

# 1. wait (up to ~2 minutes) for the driver's device node
n=0
while [ ! -e /dev/misc/iwi/0 ] && [ $n -lt 60 ]; do
	sleep 2
	n=`expr $n + 1`
done

# 2. wait (up to ~1 minute) for the network server to answer
n=0
while [ $n -lt 30 ]; do
	if ifconfig 2>/dev/null | grep loop0 > /dev/null; then break; fi
	sleep 2
	n=`expr $n + 1`
done

cd $WIFI_DIR || exit 1
echo "start-wifi: starting iwid" >> $LOG
while true; do
	./iwid -b -r >> $LOG 2>&1
	echo "start-wifi: iwid exited, restarting in 15 s" >> $LOG
	sleep 15
done
