# Weather Station

ESP32-C6 firmware (ESP-IDF 5.5, 16 MB flash) for a weather station. Work in
progress — networking, web UI and OTA infrastructure are done; actual weather
sensors are not connected yet.

## Implemented

- **Wi-Fi (STA)** — up to 5 saved networks in NVS, tried sequentially:
  5 attempts per network, 5 s pause between attempts; each attempt does an
  all-channel scan and joins the strongest BSSID for that SSID (so on a
  multi-AP network it latches onto the nearest access point), unless the
  network is pinned to a specific BSSID (picked from the scan list), in which
  case it connects only to that AP. After all networks fail (or when the store
  is empty at boot) it falls back to AP mode.
- **AP mode** — `WeatherStation` / `weather123`, used for provisioning.
  Runs as APSTA, so Wi-Fi scanning keeps working and — when enabled from an
  already-connected state — the existing STA association to the router survives
  (SoftAP just shares the STA channel): the device stays reachable on the main
  network while the AP is up. `/api/status` reports the station and the access
  point as two independent objects (`sta` and `ap`), so both are exposed
  regardless of mode, and the web UI shows them as separate sections. Toggled
  manually with the BOOT button (GPIO9, single click: AP ↔ reconnect STA).
- **Web UI** — single page embedded into the firmware, `http://weather.local`
  (mDNS). Dark dashboard: three sensor cards (temperature / humidity / CO₂)
  with current value, trend arrow, min/max and a sparkline chart, an
  outside-weather card (Open-Meteo, refreshed hourly: a decoded conditions
  line, one tile per parameter — temperature, feels-like, daily min/max,
  humidity, surface & sea-level pressure, UV index, wind speed, gusts,
  direction, cloud cover and total precipitation — and a meta line with the
  grid-cell elevation, the coordinates and the location's UTC offset; a chip row switches the
  displayed location and adds/removes them, resolving a typed city name to
  coordinates via in-browser Open-Meteo geocoding), plus a system card (Wi-Fi details, chip temp, heap, CPU load, NVS, uptime, reset
  reason, firmware/build version), a Wi-Fi card (one list — saved networks by
  default, augmented with scanned APs on Scan; saved and the connected AP are
  badged; tap an AP → password modal with an optional "pin BSSID" toggle →
  save; per-network delete; reconnect button) and a settings card (LED
  brightness, SCD40 FRC calibration).
  The system/Wi-Fi/settings cards are hidden by default behind a gear toggle
  in the header (state persists in localStorage), so the page opens as a
  compact sensor dashboard.
  Auto-refreshes every 1 s; the header connection pill turns red and the page
  dims when the device is unreachable. Bilingual (RU/EN): all strings live in
  an in-page dictionary, the header has a language switch, the choice persists
  in localStorage and defaults to the browser language.
- **OTA** — push model: `./flash-ota.sh [host]` builds and uploads the binary
  to `POST /api/ota` (auth via `X-OTA-Key` header, key defined in `ota.c`).
  Two 4 MB app partitions + otadata; rollback is enabled — a new image must
  confirm itself at the end of `app_main`, otherwise the bootloader reverts.
- **Status LED** (WS2812 on GPIO8, own FreeRTOS task, 100 ms tick). The task
  polls the other modules each tick and shows, by priority (highest first):
  - purple blinking — OTA upload in progress
  - red blinking — error, blink count = code (2 = SCD40 not responding; a
    missing reading is ignored for the first 10 s after boot while the sensor
    warms up)
  - blue — AP mode; every 3 s it pulses off N times = number of AP clients
  - green blinking — connecting to Wi-Fi
  - connected & healthy — solid, color by CO₂ level with a smooth
    green→yellow→red gradient (green ≤400, yellow 800, red ≥1200 ppm);
    dips off briefly on every HTTP request
- **SCD40 (CO₂ / temperature / humidity)** — I2C on GPIO2 (SDA) / GPIO3 (SCL),
  periodic measurement mode (fresh reading every 5 s — the sensor's maximum).
  Hot-plug: an absent/failing sensor is re-probed every 5 s, the web UI shows
  a separate "Воздух" card (or "датчик не отвечает"), `/api/status` carries
  `scd40_ok`, `co2`, `air_temp`, `air_rh`. Forced recalibration (FRC) via a
  button on the page — run the sensor ≥3 min in known-CO₂ air first.
  Pressure compensation is hardcoded (985 hPa at the site, 245 m) until a
  real pressure sensor provides live values.
- **History & charts** — SCD40 readings in three ring buffers:
  5 min of 1 s samples (RAM only), 1 h of 5 s points (the sensor's native
  rate) and 24 h of 1-min averages, ~20 KB RAM total. The two longer rings
  are snapshotted to LittleFS (`/data/hist_1h.bin` every 5 min,
  `/data/history.bin` every 10 min, both on graceful shutdown / OTA reboot)
  and restored on boot — downtime shows up as a gap, anchored via SNTP time;
  points collected before the deferred restore are merged in after the gap.
  The web UI draws CO₂/temperature/humidity sparkline charts inside the
  sensor cards with a small dependency-free canvas renderer: switchable
  window (5 min / hour / day, one switch for all charts) fetched from
  `/api/history?p=` and re-polled at 2/10/60 s respectively, time/value
  axis labels, hover crosshair with a value + time tooltip; offline periods
  show as gaps. uPlot is no longer used by the page, but its assets are
  still embedded and served at `/uplot.js` + `/uplot.css` (removal pending).
- **Telegram notifications** — push-only bot (no incoming commands yet):
  welcome message on boot (chip temp + IP + BSSID + channel + RSSI),
  notification on IP change,
  air alerts from the SCD40 — CO₂ crossing the 800/1200 ppm thresholds
  (±25 ppm hysteresis), temperature drift ≥2 °C, humidity drift ≥10 %.
  Bot token / chat id are compile-time constants in `telegram.c`
  (like `OTA_KEY`); when left empty the module disables itself.
- **Outside weather (Open-Meteo)** — outdoor conditions fetched over HTTPS
  once an hour. From the `current` block: temperature and apparent
  ("feels like") temperature, humidity, surface and sea-level pressure,
  UV index, cloud cover, wind speed / gusts / direction, total `precipitation`
  (rain + showers + snow water-equivalent) and the WMO `weather_code`; from
  the `daily` block (`forecast_days=1`) today's
  min/max temperature. `timezone=auto` aligns the daily min/max to the local
  day and yields `utc_offset_seconds`; the reply's `elevation` is kept too.
  No API key needed; the default `best_match` model picks the most accurate
  regional model for the coordinates (ICON / ECMWF over Europe). Only the
  active location is fetched; the full request URL is logged on every fetch.
  A failed fetch drops the cached reading (`weather_ok` goes false, the card
  empties — no stale values are shown) and is retried in 5 min.
  Exposed as `weather_*` fields in
  `/api/status` (incl. `weather_name` / `weather_active`) and shown in a
  dedicated "outside weather" card on the web page;
  `weather_api_code_str()` decodes the WMO code to English text (the web UI
  decodes it bilingually).
- **Weather locations** — up to 10 named locations `{name, lat, lon}` plus the
  active one are stored in NVS (`weather_store.c`). The list is empty on first
  boot — until a location is added the weather card stays empty and no fetch is
  made. The web UI switches between
  them (chips in the weather card) and adds/removes them; switching the active
  location wakes the fetch task via `weather_api_refresh()` to reload right
  away. Coordinates are resolved from a place name by the browser (Open-Meteo
  geocoding) — the device stores only the numeric result — so adding a location
  needs internet on the client (not in AP-provisioning mode). Managed through
  `GET/POST/DELETE /api/locations` and `PUT /api/locations/active`.
- **SNTP** — UTC time from `pool.ntp.org`. Sync state is exposed as
  `timesync_is_synced()` and as `time_synced` in `/api/status`; until the
  first sync `time` contains dashes instead of digits.

## Architecture

`app_main` (main.c) only initializes modules and exits; everything runs in
tasks/callbacks. Modules under `main/`, each with a small public header:

| Module | Role |
|---|---|
| `wifi.c` | connection state machine; state exposed via `wifi_get_status()` (polled by the LED task) |
| `wifi_store.c` | saved credentials in NVS (namespace `wifi_creds`), mutex-protected |
| `webserver.c` | esp_http_server + mDNS; all handlers go through one wrapper that logs the request (except `/api/status` — polled every 1 s) and blinks the LED; static buffers are safe (single httpd task) |
| `ota.c` | `POST /api/ota` handler + rollback confirmation |
| `led.c` | LED task; polls wifi/sensors/ota each tick and picks the pattern, brightness (persisted) |
| `button.c` | BOOT button via espressif/button |
| `sensors.c` | internal chip temperature + SCD40 polling task (I2C master bus, Sensirion protocol with CRC-8) |
| `timesync.c` | SNTP client; `timesync_is_synced()` flag |
| `history.c` | three RAM rings (5 min @ 1 s, 1 h @ 5 s, 24 h @ 1 min); fed by `sensors.c`, driven by a 1 s esp_timer; the two longer rings persist to `/data/hist_1h.bin` / `/data/history.bin` (5/10-min snapshots + shutdown handler) |
| `storage.c` | mounts the LittleFS `storage` partition at `/data` |
| `telegram.c` | message queue + sender task (Telegram Bot API over HTTPS); `telegram_notify(fmt, ...)` |
| `weather_api.c` | Open-Meteo client; own task fetches the active location's outdoor conditions (temp/feels-like, daily min/max, humidity, surface & MSL pressure, UVI, wind, clouds, precipitation, WMO code, elevation, UTC offset) over HTTPS once an hour, exposed via `weather_api_get()` / `weather_api_code_str()`; `weather_api_refresh()` forces an immediate reload on a location change |
| `weather_store.c` | saved weather locations `{name, lat, lon}` + active index in NVS (namespace `weather_loc`), mutex-protected; empty until the user adds a location |
| `alerts.c` | notification rules & thresholds; own task polls sensors/network every 10 s |
| `settings.c` | thin u8 get/set over NVS namespace `settings` |

## HTTP API

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | embedded single-page UI (index.html) |
| `/api/status` | GET | full status JSON: Wi-Fi (STA details or AP client list), system info, heap, settings, outdoor weather (`weather_*`) |
| `/api/history` | GET | `?p=5m\|1h\|1d` (default `1d`) selects the ring; returns `{period: <s>, co2: [...], temp: [...], rh: [...]}`, `null` = gap |
| `/api/scan` | GET | scan for Wi-Fi networks, returns `[{ssid, bssid, ch, rssi, auth}]` (one entry per BSSID — same SSID may repeat across APs) |
| `/api/networks` | GET | list of saved networks `[{ssid, bssid?}]` (bssid present only when the network is pinned to an AP) |
| `/api/networks/add` | POST | save a network; body `{"ssid": "...", "password": "...", "bssid": "aa:bb:.."}` (bssid optional — pins the connection to that AP; omit/all-zero = connect to strongest), updates existing SSID |
| `/api/networks/delete` | POST | remove a saved network; body `{"ssid": "..."}` |
| `/api/locations` | GET | saved weather locations `{"active": <idx>, "locations": [{"name", "lat", "lon"}]}` |
| `/api/locations` | POST | add a location; body `{"name": "...", "lat": <n>, "lon": <n>}` (coordinates resolved in-browser via Open-Meteo geocoding), updates an existing name |
| `/api/locations` | DELETE | remove a location; body `{"index": <n>}` |
| `/api/locations/active` | PUT | switch the displayed location; body `{"index": <n>}` (triggers an immediate refetch) |
| `/api/connect` | POST | leave AP mode / restart the STA connection cycle |
| `/api/settings` | POST | apply settings; body `{"led_brightness": 1–255}`, persisted in NVS |
| `/api/scd40/calibrate` | POST | SCD40 forced recalibration; body `{"ppm": 400–2000}`, returns applied correction |
| `/api/ota` | POST | firmware update; raw binary body, requires `X-OTA-Key` header; reboots on success |

## Build & flash

```sh
idf.py build flash monitor   # first time (partition table changed) — by cable
./flash-ota.sh               # afterwards — over the network
```

Deliberate config deviations live in `sdkconfig.defaults` (16 MB flash size,
custom partition table, rollback, run-time stats for CPU load, -Os, and
`LWIP_MAX_SOCKETS=16` so httpd + mDNS + SNTP don't starve the outbound TLS
clients of sockets); `sdkconfig` is generated and not tracked. `partitions.csv`: nvs 24K, otadata 8K, phy 4K, ota_0/ota_1
4M each, storage (LittleFS) 6M pinned to the end of flash at 0xA00000 so
future app-slot growth does not move the data.
