#!/bin/bash
# tools/umi-usbnet.sh: wait (up to 3 min) for the phone's USB Ethernet (en8) to appear after a
# fastboot boot, then give the Mac its address on the link. Run it right after starting the boot;
# it asks for the sudo password once, up front.
IF=${1:-en8}
sudo -v || exit 1
for i in $(seq 1 90); do
	if ifconfig "$IF" >/dev/null 2>&1; then
		sudo ifconfig "$IF" inet 192.168.7.1 netmask 255.255.255.0 alias && ifconfig "$IF" | grep 'inet 192'
		exit $?
	fi
	sleep 2
done
echo "$IF did not appear in 3 minutes" >&2
exit 1
