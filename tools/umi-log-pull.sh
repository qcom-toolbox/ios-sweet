#!/bin/bash
# umipull.sh NAME: pull the log ring from TWRP into research/logs/NAME.txt
set -eu; export ANDROID_SERIAL=d0ee024f
cd /Users/enzo/ios-sweet-port/ios-sweet
out=research/logs/$1.txt
dev=$(adb shell readlink -f /dev/block/by-name/userdata | tr -d '\r'); bytes=$(adb shell blockdev --getsize64 $dev | tr -d '\r'); skip=$((bytes/4096-1025))
adb exec-out "dd if=$dev bs=4096 skip=$skip count=1025 2>/dev/null" > $out.bin
python3 tools/sweet-log.py $out.bin $out | head -2
