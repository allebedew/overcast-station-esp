#!/usr/bin/env bash
# Build and flash over the network: ./flash-ota.sh [host]
# Default host is weather.local (mDNS).
set -euo pipefail
cd "$(dirname "$0")"

HOST="${1:-weather.local}"
OTA_KEY="weather-ota" # same key as in main/ota.c

# build.sh sources the ESP-IDF environment itself and fails if it is missing,
# so no exported shell is needed here.
./build.sh

echo "Uploading to http://$HOST/api/ota ..."
curl --fail --progress-bar --max-time 180 \
     -H "X-OTA-Key: $OTA_KEY" \
     --data-binary @build/station.bin \
     -o /dev/null \
     "http://$HOST/api/ota"

echo "Done — the device is rebooting into the new firmware."
