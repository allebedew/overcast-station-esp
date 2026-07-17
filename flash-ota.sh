#!/usr/bin/env bash
# Сборка и прошивка по сети: ./flash-ota.sh [host]
# По умолчанию host = weather.local (mDNS).
set -euo pipefail
cd "$(dirname "$0")"

HOST="${1:-weather.local}"
OTA_KEY="weather-ota" # тот же, что в main/ota.c

if ! command -v idf.py >/dev/null; then
    echo "idf.py не найден — сначала: . \$IDF_PATH/export.sh" >&2
    exit 1
fi

idf.py build

echo "Загрузка на http://$HOST/api/ota ..."
curl --fail --progress-bar --max-time 180 \
     -H "X-OTA-Key: $OTA_KEY" \
     --data-binary @build/station.bin \
     -o /dev/null \
     "http://$HOST/api/ota"

echo "Готово — устройство перезагружается в новую прошивку."
