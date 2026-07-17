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
- **SCD40 (CO₂ / temperature / humidity)** — I2C on GPIO2 (SDA) / GPIO3 (SCL),
  periodic measurement mode (fresh reading every 5 s — the sensor's maximum).
  Hot-plug: an absent/failing sensor is re-probed every 5 s, the web UI shows
  a separate "Воздух" card (or "датчик не отвечает"), `/api/status` carries
  `scd40_ok`, `co2`, `air_temp`, `air_rh`. Forced recalibration (FRC) via a
  button on the page — run the sensor ≥3 min in known-CO₂ air first.
  Pressure compensation is hardcoded (985 hPa at the site, 245 m) until a
  real pressure sensor provides live values.
- **History & charts** — 24 h of SCD40 readings averaged into 1-min points
  (RAM ring, ~9 KB). The ring is snapshotted to LittleFS every 10 min and on
  graceful shutdown (OTA reboot), and restored on boot — downtime shows up
  as a gap, anchored via SNTP time. The web UI draws interactive
  CO₂/temperature/humidity charts with uPlot (embedded into the firmware,
  served as `/uplot.js` + `/uplot.css`): switchable window (5 min live from
  the 1 s status polling / hour / day, one switch for all charts), hover
  readout of time/value; offline periods show as gaps.
- **Telegram notifications** — push-only bot (no incoming commands yet):
  welcome message on boot (chip temp + IP), notification on IP change,
  air alerts from the SCD40 — CO₂ crossing the 800/1200 ppm thresholds
  (±25 ppm hysteresis), temperature drift ≥2 °C, humidity drift ≥10 %.
  Bot token / chat id are compile-time constants in `telegram.c`
  (like `OTA_KEY`); when left empty the module disables itself.
- **SNTP** — UTC time from `pool.ntp.org`. Sync state is exposed as
  `timesync_is_synced()` and as `time_synced` in `/api/status`; until the
  first sync `time` contains dashes instead of digits.

## Architecture

`app_main` (main.c) only initializes modules and exits; everything runs in
tasks/callbacks. Modules under `main/`, each with a small public header:

| Module | Role |
|---|---|
| `wifi.c` | connection state machine, events → status callback (used by main.c to drive the LED) |
| `wifi_store.c` | saved credentials in NVS (namespace `wifi_creds`), mutex-protected |
| `webserver.c` | esp_http_server + mDNS; all handlers go through one wrapper that logs the request (except `/api/status` — polled every 2 s) and blinks the LED; static buffers are safe (single httpd task) |
| `ota.c` | `POST /api/ota` handler + rollback confirmation |
| `led.c` | LED task; status enum, brightness (persisted) |
| `button.c` | BOOT button via espressif/button |
| `sensors.c` | internal chip temperature + SCD40 polling task (I2C master bus, Sensirion protocol with CRC-8) |
| `timesync.c` | SNTP client; `timesync_is_synced()` flag |
| `history.c` | 24 h RAM ring of 1-min averaged sensor points; fed by `sensors.c`, flushed by an esp_timer once a minute; persisted to `/data/history.bin` (10-min snapshots + shutdown handler) |
| `storage.c` | mounts the LittleFS `storage` partition at `/data` |
| `telegram.c` | message queue + sender task (Telegram Bot API over HTTPS); `telegram_notify(fmt, ...)` |
| `alerts.c` | notification rules & thresholds; own task polls sensors/network every 10 s |
| `settings.c` | thin u8 get/set over NVS namespace `settings` |

## HTTP API

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | embedded single-page UI (index.html) |
| `/api/status` | GET | full status JSON: Wi-Fi (STA details or AP client list), system info, heap, settings |
| `/api/history` | GET | 24 h of 1-min points: `{period, co2: [...], temp: [...], rh: [...]}`, `null` = gap |
| `/api/scan` | GET | scan for Wi-Fi networks, returns `[{ssid, rssi, auth}]` |
| `/api/networks` | GET | list of saved networks (SSIDs only) |
| `/api/networks/add` | POST | save a network; body `{"ssid": "...", "password": "..."}`, updates password if SSID exists |
| `/api/networks/delete` | POST | remove a saved network; body `{"ssid": "..."}` |
| `/api/connect` | POST | leave AP mode / restart the STA connection cycle |
| `/api/settings` | POST | apply settings; body `{"led_brightness": 1–255}`, persisted in NVS |
| `/api/scd40/calibrate` | POST | SCD40 forced recalibration; body `{"ppm": 400–2000}`, returns applied correction |
| `/api/ota` | POST | firmware update; raw binary body, requires `X-OTA-Key` header; reboots on success |

## Build & flash

```sh
idf.py build flash monitor   # first time (partition table changed) — by cable
./flash-ota.sh               # afterwards — over the network
```

Deliberate config deviations live in `sdkconfig.defaults` (custom partition
table, rollback, run-time stats for CPU load, -Os); `sdkconfig` is generated
and not tracked. `partitions.csv`: nvs 24K, otadata 8K, phy 4K, ota_0/ota_1
4M each, storage (LittleFS) 6M pinned to the end of flash at 0xA00000 so
future app-slot growth does not move the data.
