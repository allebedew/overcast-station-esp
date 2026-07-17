# Weather Station

ESP32-C6 firmware (ESP-IDF 5.5, 16 MB flash) for a weather station. Work in
progress — networking, web UI and OTA infrastructure are done; actual weather
sensors are not connected yet.

## Implemented

- **Wi-Fi (STA)** — up to 5 saved networks in NVS, tried sequentially:
  5 attempts per network, 5 s pause between attempts. After all networks fail
  (or when the store is empty at boot) the device falls back to AP mode.
- **AP mode** — `WeatherStation` / `weather123`, used for provisioning.
  Runs as APSTA so Wi-Fi scanning keeps working. Toggled manually with the
  BOOT button (GPIO9, single click: AP ↔ reconnect STA).
- **Web UI** — single page embedded into the firmware, `http://weather.local`
  (mDNS). Shows Wi-Fi details, system info (chip temp, heap, CPU load, NVS,
  uptime, reset reason, firmware/build version), manages saved networks
  (scan / add / delete / reconnect) and settings (LED brightness).
  Auto-refreshes every 2 s, shows a banner when the device is unreachable.
- **OTA** — push model: `./flash-ota.sh [host]` builds and uploads the binary
  to `POST /api/ota` (auth via `X-OTA-Key` header, key defined in `ota.c`).
  Two 4 MB app partitions + otadata; rollback is enabled — a new image must
  confirm itself at the end of `app_main`, otherwise the bootloader reverts.
- **Status LED** (WS2812 on GPIO8, own FreeRTOS task, 250 ms tick):
  - yellow blinking — connecting; yellow solid — waiting between retries
  - green — connected; blinks off briefly on every HTTP request
  - red — error; purple blinking — OTA upload in progress
  - blue — AP mode; every 3 s it pulses off N times = number of AP clients
- **SNTP** — UTC time from `pool.ntp.org`.

## Architecture

`app_main` (main.c) only initializes modules and exits; everything runs in
tasks/callbacks. Modules under `main/`, each with a small public header:

| Module | Role |
|---|---|
| `wifi.c` | connection state machine, events → status callback (used by main.c to drive the LED) |
| `wifi_store.c` | saved credentials in NVS (namespace `wifi_creds`), mutex-protected |
| `webserver.c` | esp_http_server + mDNS; all handlers go through one wrapper that logs the request and blinks the LED; static buffers are safe (single httpd task) |
| `ota.c` | `POST /api/ota` handler + rollback confirmation |
| `led.c` | LED task; status enum, brightness (persisted) |
| `button.c` | BOOT button via espressif/button |
| `sensors.c` | internal chip temperature (placeholder for real sensors) |
| `settings.c` | thin u8 get/set over NVS namespace `settings` |

## HTTP API

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | embedded single-page UI (index.html) |
| `/api/status` | GET | full status JSON: Wi-Fi (STA details or AP client list), system info, heap, settings |
| `/api/scan` | GET | scan for Wi-Fi networks, returns `[{ssid, rssi, auth}]` |
| `/api/networks` | GET | list of saved networks (SSIDs only) |
| `/api/networks/add` | POST | save a network; body `{"ssid": "...", "password": "..."}`, updates password if SSID exists |
| `/api/networks/delete` | POST | remove a saved network; body `{"ssid": "..."}` |
| `/api/connect` | POST | leave AP mode / restart the STA connection cycle |
| `/api/settings` | POST | apply settings; body `{"led_brightness": 1–255}`, persisted in NVS |
| `/api/ota` | POST | firmware update; raw binary body, requires `X-OTA-Key` header; reboots on success |

## Build & flash

```sh
idf.py build flash monitor   # first time (partition table changed) — by cable
./flash-ota.sh               # afterwards — over the network
```

Deliberate config deviations live in `sdkconfig.defaults` (custom partition
table, rollback, run-time stats for CPU load, -Os); `sdkconfig` is generated
and not tracked. `partitions.csv`: nvs 24K, otadata 8K, phy 4K, ota_0/ota_1
4M each.
