#!/bin/bash
# Copy doom_DEVICE.pdx to a USB-connected Playdate and launch it.
# The SDK's pdutil has no "install": mount the data disk, copy into Games/, eject, run.
set -e
cd "$(dirname "$0")/.."
PDUTIL="$PLAYDATE_SDK_PATH/bin/pdutil"
PDX=doom_DEVICE.pdx
VOL=/Volumes/PLAYDATE

find_port() { ls /dev/cu.usbmodemPD* 2>/dev/null | head -n 1; }

PD=$(find_port)
if [ -z "$PD" ]; then
  echo "No Playdate serial port found. Unlock the device and connect it via USB."
  exit 1
fi

echo "Mounting data disk via $PD ..."
"$PDUTIL" "$PD" datadisk

for _ in $(seq 1 30); do [ -d "$VOL/Games" ] && break; sleep 1; done
if [ ! -d "$VOL/Games" ]; then
  echo "$VOL/Games did not appear. If the Playdate is showing 'Data Disk', mount it manually and retry."
  exit 1
fi

echo "Copying $PDX ..."
# cp, not rsync: macOS rsync fails to mkdir on the FAT data disk.
rm -rf "$VOL/Games/$PDX"
cp -RX "$PDX" "$VOL/Games/$PDX"
sync
diskutil eject "$VOL"

echo "Launching ..."
for _ in $(seq 1 20); do PD=$(find_port); [ -n "$PD" ] && break; sleep 1; done
[ -n "$PD" ] && "$PDUTIL" "$PD" run "/Games/$PDX" || echo "Copied. Launch it from the Playdate's home screen."
