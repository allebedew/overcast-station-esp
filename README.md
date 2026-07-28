# Weather Station

ESP32-C6 firmware (ESP-IDF 5.5, 16 MB flash) for a weather station. Work in
progress — networking, web UI and OTA infrastructure are done; the I2C sensors
report into `/api/status`, the web page and the LCD, but only the SCD40 feeds
the history, the charts and the alerts so far.

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
  manually by holding the BOOT button (GPIO9) for 1.5 s: AP ↔ reconnect STA.
  A short click of the same button browses the display pages.
- **Web UI** — single page embedded into the firmware (gzipped at build time,
  served with `Content-Encoding: gzip`), `http://weather.local`
  (mDNS). Dark dashboard: three sensor cards (temperature / humidity / CO₂)
  with current value, trend arrow, min/max and a sparkline chart, then a
  sensors card holding one equal-height nested card per I2C device — SCD40,
  TMP117, BMP581, VEML7700 — each with an online/no-response badge and its
  readings as tiles built like the weather ones (the VEML7700's raw counts,
  gain and integration time sit on a footer line instead: they explain the lux
  values rather than being readings); an offline device keeps its card, dimmed,
  with placeholder values. Below them an
  outside-weather card (Open-Meteo, refreshed hourly: a decoded conditions
  line, one tile per parameter — temperature, feels-like, daily min/max,
  humidity, surface & sea-level pressure, UV index, wind speed, gusts,
  direction, cloud cover and total precipitation — and a meta line with the
  grid-cell elevation, the coordinates and the location's UTC offset; a chip row switches the
  displayed location and adds/removes them, resolving a typed city name to
  coordinates via in-browser Open-Meteo geocoding), plus a
  system card (Wi-Fi details, chip temp, heap, CPU load, NVS, uptime, reset
  reason, firmware/build version), a Wi-Fi card (one list — saved networks by
  default, augmented with scanned APs on Scan; saved and the connected AP are
  badged; tap an AP → password modal with an optional "pin BSSID" toggle →
  save; per-network delete; reconnect button) and a settings card (LED
  brightness, LCD backlight hue — a rainbow slider with −/+ buttons stepping
  5° at a time, wrapping around the circle — LCD backlight brightness, and
  SCD40 FRC calibration). Hue/brightness are a browser-side way to pick a
  color: the page converts them to RGB (HSV at full saturation), previews the
  result as a color dot, and sends the device only the finished value; on load
  the sliders are derived back from the stored color. A hex field next to them
  shows the current value and accepts a typed one (`00AAFF`, a leading `#` is
  fine) — that path covers colors the two sliders cannot express, such as
  white, and a typed value is sent verbatim.
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
    missing reading is ignored for the first 12 s after boot, while the sensor
    walks its start sequence and produces a first measurement)
  - blue — AP mode; every 3 s it pulses off N times = number of AP clients
  - green blinking — connecting to Wi-Fi
  - connected & healthy — solid, color by CO₂ level with a smooth
    green→yellow→red gradient (green ≤400, yellow 800, red ≥1200 ppm);
    dips off briefly on every HTTP request
- **I2C sensors** — everything on the bus (SDA GPIO2 / SCL GPIO3) lives in
  `main/sensors/`: the bus itself, one transport file per device, and a single
  polling task in `sensors.c` that ticks at 100 ms and brings each sensor up at
  its own period through one shared hot-plug state machine — probe until the
  device answers, then read; 3 consecutive failures drop it back to probing
  every 5 s. Each device appears in `/api/status` as its own object carrying
  `ok` plus its readings, and the web UI keeps its card either way (dimmed,
  with placeholders, when it is offline). Setup failures are diagnosed from the
  log, not the API: each driver names the address, the step it gave up on
  (`probe`, `attach`, `chip_id`/`device_id`, `id_mismatch`, `reset`, `nvm`,
  `iir`, `dsp`, `osr`, `odr`, `config`, …) and the I2C error.
  The bus lock lives with the bus (`i2c_bus.c`) rather than inside the sensor
  module, because a command-then-read pair, or an address-then-data pair on the
  display, has to stay unbroken; the lock is recursive, so a driver-level
  helper and a caller bracketing a longer sequence compose. Waits that the
  *device* needs are deliberately left outside it — a start sequence with a
  half-second settling step in it returns `ESP_ERR_NOT_FINISHED` and resumes at
  the next poll, with the bus free meanwhile.
  Sub-millisecond-to-few-ms waits inside the drivers are busy-waits
  (`esp_rom_delay_us`): the FreeRTOS tick is 10 ms, so
  `vTaskDelay(pdMS_TO_TICKS(n))` for a few ms rounds down to zero ticks and
  does not delay at all.
  Only the SCD40 is consumed by anything beyond the API and the display — the
  other three do not feed the history, the charts or the alerts yet.
  - **SCD40** (`0x62`) — CO₂ / temperature / humidity, periodic measurement
    mode (fresh reading every 5 s, the sensor's maximum). The result appears at
    a phase the firmware does not know until it has caught one, so the poll
    checks once a second until then and afterwards goes quiet for 4.5 s —
    the same data for a fifth of the bus traffic. Forced recalibration (FRC)
    via a button on the page — run the sensor ≥3 min in known-CO₂ air first.
    Automatic self-calibration (ASC) is switched **off** at startup: it assumes
    the sensor sees outdoor air as its weekly minimum, which a room that is
    never aired to 400 ppm does not deliver, and this station calibrates by
    hand instead. That costs one EEPROM write, so the setting is read first and
    only cleared when it is actually on — once in the sensor's life.
    Pressure compensation comes from the BMP581, re-sent whenever the measured
    value drifts 2 hPa from what the sensor was last told (and after every
    start, since the setting is lost on power-down); a reading outside
    700–1200 hPa, or no BMP581 at all, falls back to 985 hPa — the value at the
    site, 245 m above sea level.
  - **TMP117** (`0x48`–`0x4B`, address auto-detected) — ±0.1 °C temperature,
    **read at 2 Hz**, running continuous conversion with 8 averaged samples and
    a 125 ms cycle. That is four results per poll: every poll finds a fresh one
    whatever jitter the scheduler adds, and the averaging takes the noise down
    by roughly a factor of three, which is what the display's second decimal
    needs. (Converting at the shortest cycle, 15.5 ms with averaging off, would
    throw away 31 of every 32 results and show the noisier single sample.) The
    device ID (`0x117`) is verified at start, so a foreign chip on one of those
    addresses is skipped rather than misread.
  - **BMP581** (`0x47` or `0x46`; the register-compatible BMP580 works too) —
    pressure and temperature, read once a second, normal mode at **4 Hz**,
    oversampling x16 on pressure and x2 on temperature. Four conversions per
    poll: running the sensor at the polling rate instead would leave the two
    clocks free-running against each other, so a poll would sometimes refetch
    the sample it already had and sometimes skip one. The pressure IIR filter
    is on at coefficient 3 (`DSP_IIR`), with `shdw_sel_iir_p` set so the
    filtered value is what the data registers hold — at 4 Hz that is a time
    constant near a second, enough to settle the last displayed digit, which
    otherwise sits below the sensor's noise floor. Temperature is left
    unfiltered. The soft reset at the start of the setup drops the part into
    deep standby, where it NACKs everything, so the next step pokes ODR_CONFIG
    (with `deep_dis` set, still in standby) every 2 ms until it answers — that
    both waits out the boot and is what actually leaves deep standby. Both
    readings come from one 6-byte burst at `0x1D`: temperature XLSB/LSB/MSB
    (`0x1D`-`0x1F`), then pressure XLSB/LSB/MSB (`0x20`-`0x22`), little-endian,
    24-bit, `temp / 65536` °C and `press / 64` Pa.
  - **VEML7700** (`0x10`, fixed) — ambient light, read once a second. The
    usable range spans six decades, so the driver auto-ranges over a nine-step
    gain / integration-time table (x1/8 @ 25 ms … x2 @ 800 ms). The needed step
    is computed from the measured level, so a reading far outside the current
    range moves straight there instead of walking the table one poll at a time;
    the sample that triggered the change is dropped. After any configuration
    change — including the one at startup — the driver waits out two
    integration periods before trusting the data register: reading it earlier
    returns the leftovers of the previous settings, which looked like darkness
    and used to send the auto-ranging climbing for no reason. Both the
    lux-matched (ALS) and the unfiltered white channel are reported, in lux and
    raw counts, together with the gain and integration time in effect. Above
    1000 lx the Vishay fourth-order linearity correction is applied — to the
    ALS channel it is calibrated for; the white channel's lux figure shares the
    same scaling and is an estimate, its raw counts being the honest number.
- **16x2 LCD** (DFRobot Gravity I2C LCD1602 RGB, DFR0464) — shares the sensor
  I2C bus (GPIO2/GPIO3) and its lock; LCD controller at `0x3E`, RGB backlight driver at
  whichever of `0x60` / `0x30` / `0x6B` / `0x2D` the board revision uses (all
  four are probed, none collides with the SCD40 at `0x62`). Four pages,
  switched by a short click of the BOOT button and remembered across reboots
  (NVS `settings/screen_page`):
  - indoor + headline — `23.4° 45% 1250` / `12.3° Overcast` (SCD40 temperature,
    humidity, CO₂; outdoor temperature and the decoded WMO description, cut to
    the room left)
  - indoor precise — `23.46° 1013.250` / `1250ppm 45% 78x` (TMP117 °C and
    BMP581 hPa at the resolution the sensors actually deliver; below them SCD40
    CO₂ and humidity plus VEML7700 illuminance, switched to kilolux past
    9999 lx). Each value is rendered on its own, so one absent sensor shows as
    `--` and leaves the rest readable
  - outdoor detail — `12.3° 8..17°U3` / `78% 1013 NW15 3` (current temperature,
    today's min/max, UV index; humidity %, sea-level pressure hPa, wind as an
    8-point compass label + km/h, WMO code)

  - system — `17:27:40 28.07` / `12% 184k -58dBm` (time in the active
    location's timezone with the colons blinking once a second as the seconds
    hand, day and month — the year does not fit alongside the seconds; below
    them CPU load over the last second, free heap in KiB and the Wi-Fi signal,
    `AP` while the station serves its own network, `--` with no link at all)

  The backlight color is stored and applied as plain RGB (NVS `bl_rgb`,
  default `00AAFF`); the device knows nothing about hue or color models.
  Redrawn at 60 fps, paced by an `esp_timer` (the 10 ms FreeRTOS tick is too
  coarse for a 16.7 ms frame); that is the polling rate — the transport skips
  frames that would repaint identical characters, so the actual bus traffic
  follows how often the readings change. Missing data shows as
  `no sensor data` / `no weather data`. Hot-plug: an absent display is
  re-probed every 5 s. Text is ASCII-only — the character ROM has no Cyrillic,
  so other bytes show as `?`.
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
  show as gaps. No charting library is bundled.
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
  A failed fetch drops the cached reading (`weather.ok` goes false, the card
  empties — no stale values are shown) and is retried in 5 min.
  Exposed as the `weather` object in
  `/api/status` (incl. `name` / `active`) and shown in a
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
  `timesync_is_synced()` and as `system.time_synced` in `/api/status`; until
  the first sync `system.time` contains dashes instead of digits.

## Architecture

`app_main` (main.c) only initializes modules and exits; everything runs in
tasks/callbacks. Modules under `main/`, each with a small public header;
everything that talks to a device on the I2C bus sits in `main/sensors/`
(added to the component's `INCLUDE_DIRS`, so the includes stay flat):

| Module | Role |
|---|---|
| `wifi.c` | connection state machine; STA state and AP state are separate fields of one snapshot from `wifi_get_info()` (polled by the LED task), with `wifi_is_connected()` as the shorthand and `wifi_ap_enable()` to toggle the AP |
| `wifi_store.c` | saved credentials in NVS (namespace `wifi_creds`), mutex-protected |
| `webserver.c` | esp_http_server + mDNS; all handlers go through one wrapper that logs the request (except `/api/status` — polled every 1 s) and blinks the LED; static buffers are safe (single httpd task) |
| `ota.c` | `POST /api/ota` handler + rollback confirmation |
| `led.c` | LED task; polls wifi/sensors/ota each tick and picks the pattern, brightness (persisted) |
| `button.c` | BOOT button via espressif/button: click → next display page, 1.5 s hold → AP toggle |
| `sensors/sensors.c` | one task polling all four sensors, each at its own period, through a shared hot-plug state machine; owns the published snapshots and the cross-sensor wiring (SCD40 readings into `history.c`, BMP581 pressure into the SCD40's compensation) |
| `sensors/i2c_bus.c` | the I2C master bus and the recursive lock that arbitrates it, for sensors and display alike; `i2c_bus_scan()` logs every responding address, run once at startup |
| `sensors/i2c_dev.c` | the register access the drivers were each repeating: attach, probe, raw transfers, u8/u16 register reads and writes in both byte orders |
| `sensors/scd40.c` | SCD40 transport: Sensirion command protocol with CRC-8, the phased start sequence (stop → ASC check → run), pressure compensation, FRC |
| `sensors/tmp117.c` | TMP117 transport: address auto-detection (`0x48`–`0x4B`), device-ID check, config (continuous, 8 averaged samples, 125 ms cycle), temperature register |
| `sensors/bmp581.c` | BMP581/BMP580 transport: address auto-detection (`0x47`/`0x46`), chip-ID check, soft reset, DSP/IIR + OSR/ODR setup, 6-byte burst read |
| `sensors/veml7700.c` | VEML7700 transport: little-endian command registers, the auto-ranging gain / integration-time table with its settle deadline, lux conversion with the >1000 lx correction |
| `chip_temp.c` | the SoC's own temperature sensor — not on the bus and measuring the die rather than the room, so it is reported with the health readings, not the sensors |
| `screen_16x2.c` | what the display shows: three pages advanced by the button (selection persisted), 60 fps frame timer, backlight as a stored RGB value. Sized for 16x2 — a bigger display gets its own layout module |
| `lcd1602_rgb.c` | transport for the DFR0464 panel: character output, backlight registers, revision detection, hot-plug recovery. Takes the bus and its lock from `i2c_bus.c` like the sensors do. The only file tied to this particular display |
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
| `/` | GET | embedded single-page UI (index.html, gzipped) |
| `/api/status` | GET | full status JSON, grouped into objects: `sta` / `ap` (Wi-Fi), `sensors`, `weather` (Open-Meteo), `system` (firmware, chip, heap, NVS, uptime, clock), `settings` (`led_brightness`, `backlight_rgb`); nothing is left at the top level. `sensors` holds one object per I2C device, each with its own `ok` — `scd40` (`co2`, `temp`, `rh`), `tmp117` (`temp`), `bmp581` (`press` hPa, `press_pa`, `temp`), `veml7700` (`lux`, `white`, `als_raw`, `white_raw`, `gain`, `it`) — plus `chip_temp` (the SoC sensor, not on the bus) at the top of the group |
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
| `/api/settings` | POST | apply settings; body `{"led_brightness": 1–255, "backlight_rgb": "RRGGBB"}` (hex string, leading `#` tolerated), any subset, all persisted in NVS |
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
