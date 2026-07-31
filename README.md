# Weather Station

ESP32-C6 firmware (ESP-IDF 5.5, 16 MB flash). Networking, web UI and OTA are
done; all four I2C sensors feed `/api/status`, the web page, the LCD and the
history. The alerts are the only consumer still tied to the SCD40 alone.

## Implemented

- **Wi-Fi (STA)** — up to 5 networks in NVS, tried in order, 5 attempts each
  5 s apart. Every attempt scans all channels and joins the strongest BSSID for
  the SSID unless the network is pinned to one. All failing (or an empty store)
  falls back to AP mode.
- **AP mode** — `WeatherStation` / `weather123`. Runs as APSTA on the STA
  channel, so an existing router association survives. `sta` and `ap` are
  independent objects in `/api/status`. BOOT (GPIO9): 1.5 s hold toggles
  AP ↔ STA reconnect, short click browses the display pages.
- **Web UI** — single page embedded in the firmware (gzipped at build time),
  `http://weather.local` (mDNS), polled every 1 s, bilingual RU/EN from an
  in-page dictionary. Five sensor cards (temperature, humidity, CO₂, pressure,
  illuminance) with trend, min/max and a sparkline; a card per I2C device with
  a liveness dot; an outside-weather card with a location chip row (a typed
  city name is geocoded in-browser, so adding one needs internet on the
  client); system, settings and Wi-Fi cards behind the header gear. Settings:
  LED brightness, backlight color, site altitude, SCD40 FRC, history reset.
  Every reading is a fixed slot generated from the tables at the top of the
  script (`SERIES`, `WEATHER_TILES`, `SENSOR_DEVS`) and renders as `--` when
  absent — adding a metric is a table entry, not markup. The altitude field is
  written back from the device only once per page load, so the poll cannot
  overwrite typing.
- **OTA** — push model: `./flash-ota.sh [host]` builds and uploads to
  `POST /api/ota` (`X-OTA-Key` header, key in `ota.c`). Two 4 MB app partitions
  + otadata; rollback enabled — a new image confirms itself at the end of
  `app_main` or the bootloader reverts.
- **Status LED** (WS2812 on GPIO8, 100 ms tick), by priority: purple blinking —
  OTA; red blinking — error, blink count = code (2 = SCD40 silent, ignored for
  the first 12 s after boot); blue — AP mode, pulsing off N times every 3 s for
  N clients; green blinking — connecting; solid — connected, color by CO₂
  (green ≤400, yellow 800, red ≥1200 ppm), dipping off on every HTTP request.
- **I2C sensors** — SDA GPIO2 / SCL GPIO3, all of it in `main/sensors/`: the
  bus, one transport file per device, and a single task ticking at 10 ms that
  brings each sensor up at its own period through one hot-plug state machine —
  probe until it answers, then read; 3 consecutive failures drop it back to
  probing every 5 s. Setup failures are diagnosed from the log: each driver
  names the address, the step it gave up on and the I2C error.
  The bus lock lives in `i2c_bus.c` and is recursive, so a driver helper and a
  caller bracketing a longer sequence compose. Waits the *device* needs stay
  outside it: a step returns `ESP_ERR_NOT_FINISHED` and resumes on the next
  10 ms tick, not the sensor's own period, with the bus free meanwhile. Waits
  of a few ms inside drivers are busy-waits (`esp_rom_delay_us`) — the FreeRTOS
  tick is 10 ms, so a short `vTaskDelay` would not delay at all.
  - **SCD40** (`0x62`) — CO₂ / temperature / humidity, periodic mode, a result
    every 5 s at an unknown phase: the poll checks once a second until it
    catches one, then sleeps 4.5 s. Automatic self-calibration is switched
    **off** at startup (read first, cleared only when actually on — it costs an
    EEPROM write); calibration is by hand, via FRC from the web page after
    ≥3 min in known-CO₂ air. Pressure compensation from the BMP581, re-sent
    after every start and on a 2 hPa drift; outside 700–1200 hPa, or with no
    BMP581, it falls back to 985 hPa (the site, 245 m above sea level).
  - **TMP117** (`0x48`–`0x4B`, auto-detected, device ID verified) — read at
    **4 Hz**, continuous conversion with 8 averaged samples on a 125 ms cycle,
    the fastest cycle averaging allows. The averaging is what the display's
    second decimal needs.
  - **BMP581** (`0x47`/`0x46`; the register-compatible BMP580 works too) — read
    at **4 Hz**, normal mode at **15 Hz**, oversampling x16 pressure / x2
    temperature (the part silently drops oversampling above ~25 Hz, so
    `OSR_EFF` is checked). Pressure IIR at coefficient 15, which at 15 Hz is a
    time constant near a second; temperature unfiltered.
  - **VEML7700** (`0x10`) — **8 Hz** or slower where the integration time says
    so, down to **1.25 Hz** at 800 ms. Auto-ranges over a nine-step gain /
    integration-time table (x1/8 @ 25 ms … x2 @ 800 ms), jumping straight to
    the step the measured level calls for; after any configuration change it
    waits out two integration periods before trusting the data register. Both
    ALS and white channels are read, with the Vishay correction above 1000 lx.
    The API reports `lux`, the white/lux ratio as a light-source signature, and
    both raw counts.
- **16x2 LCD** (DFRobot Gravity I2C LCD1602 RGB, DFR0464) — shares the sensor
  bus and its lock; controller at `0x3E`, backlight driver at whichever of
  `0x60` / `0x30` / `0x6B` / `0x2D` the board revision uses. Redrawn at 10 fps;
  an absent panel is re-probed every 5 s. Text is ASCII-only — the ROM has no
  Cyrillic and other bytes show as `?`, the exceptions being the degree sign
  (`\xDF`) and the solid block (`\xFF`). Four pages, advanced by a short BOOT
  click and remembered in NVS (`settings/screen_page`):

  | Page | Rows |
  |---|---|
  | indoor + headline | `23.46° 45% 1250` / `12.3°  Overcast` |
  | indoor precise | `23.46° 16.8 1250` / `1013.250  999.9x` |
  | outdoor detail | `12.3° 8..17 C3` / `78% 1013 NW15 U3` |
  | system | `17:27:40 28.07` / `12% 184k BL184` |

  Sixteen characters are the whole constraint: the humidity is whole percent on
  the headline page and gives up its slot to the dew point on the precise one,
  and the illuminance keeps a tenth of a lux only below
  1000 lx, then drops to whole lux and to kilolux — the one place in the
  firmware that shows fewer digits than the standard resolution.

  Two pages outside that rotation pre-empt whatever is selected; the button
  cannot reach them and neither is stored, so the chosen page returns.
  - Wi-Fi connect — `Wi-Fi connect...` / `HomeNetwork`, only the **first**
    connection after a restart. Once the station associates, or the round-robin
    gives up, it steps aside for good and a later drop is reported by the LED
  - OTA update — `Updating...  67%` over a progress bar, one cell per 6.25 %.
    Stays at 100 % until the reboot, disappears on a failed upload

  The backlight color is stored as plain RGB (NVS `bl_rgb`, default `00AAFF`) —
  the device knows nothing about hue or color models. What reaches the panel is
  that color scaled by the ambient light, 0-255: mapped logarithmically from
  1 lx and below, where the scale sits at its floor of 10 (dim, never dark), up
  to 70 lx and above, where the color goes out untouched. A lit channel stays
  lit at the bottom end. No setting; a missing reading means full brightness.
  The scale eases into a new level a few units per frame instead of jumping, so
  a passing shadow does not flicker the panel.
- **Climate** — the room as opposed to the chips. One source per quantity and
  **no fallbacks**: temperature from the TMP117, humidity and CO₂ from the
  SCD40, pressure from the BMP581, illuminance from the VEML7700. The SCD40 and
  BMP581 temperatures are ignored — a sensor standing in for a missing TMP117
  would put a step of about a degree into the charts, indistinguishable from a
  real event. A quantity with no sensor behind it is absent, never zero. Pure
  composition: no task, no state, no lock. Read by both the cards and the
  history, so a card and the chart under it cannot disagree.
  Alongside the measured pressure it carries the same reading **reduced to sea
  level** (international barometric formula, site altitude its only input; at
  altitude 0 the factor is exactly 1). Every readout shows the reduced value,
  the rings store the pressure **as measured** and `/api/history` reduces on
  the way out — so a later altitude correction re-reduces the whole history.
  The **dew point** (Magnus-Tetens) is the one derived quantity, from the
  SCD40's own temperature and humidity — its warm offset cancels there, and the
  driver computes it. Follows the humidity. Not in the history, so it gets no
  main card — those are card-and-chart pairs; it shows on the SCD40 card and on
  the precise LCD page in place of the humidity.
- **Site altitude** — metres above sea level, −500…9000, NVS
  `settings/altitude_m` (default 0). A wrong altitude shifts every pressure
  readout and chart, never the stored history.
- **Reading resolution** — one per quantity, the same in the web page, the API,
  the charts, the rings and the LCD. Written down in `climate.h`. These are
  resolutions, not accuracies (the parts are worth ±0.1 °C, ±0.3 hPa,
  ±(50 ppm + 5 %), ±6 %RH); they buy identical readings everywhere and charts
  that are not quantised into flat steps.

  | Quantity | Resolution | Sensor | Format |
  |---|---|---|---|
  | temperature | 0.01 °C | TMP117 | `%.2f` |
  | pressure | 0.001 hPa | BMP581 | `%.3f` |
  | CO₂ | 1 ppm | SCD40 | `%u` |
  | humidity | 0.1 % | SCD40 | `%.1f` |
  | dew point | 0.1 °C | SCD40 | `%.1f` |
  | illuminance | 0.1 lx | VEML7700 | `%.1f` |

- **History & charts** — five quantities in three rings: 5 min of 1 s samples
  (RAM only), 1 h of 5 s points, 24 h of 1-min averages — 38 KB plus a 23 KB
  staging buffer. Filled by **sampling** `climate_get()` once a second, so each
  quantity is recorded for as long as its own sensor lives and validity is per
  quantity. Points are 16 bytes: fixed-point for the four bounded quantities,
  float for illuminance.
  The two longer rings are snapshotted to LittleFS (`/data/hist_1h.bin` every
  5 min, `/data/history.bin` every 10 min, both on graceful shutdown / OTA
  reboot) and restored on boot; downtime shows as a gap anchored via SNTP time.
  The header is versioned and a file from older firmware is dropped.
  Charts are a dependency-free canvas sparkline per card: window 5 min / 1 h /
  1 d from `/api/history?p=`, re-polled at 2/10/60 s, with a hover crosshair
  and gaps for offline periods. Illuminance is drawn on a **logarithmic** y
  axis.
- **Telegram notifications** — push-only bot: welcome on boot, IP change, and
  air alerts from the SCD40 — CO₂ crossing 800/1200 ppm (±25 ppm hysteresis),
  temperature drift ≥2 °C, humidity drift ≥10 %. Token and chat id are
  compile-time constants in `telegram.c`; left empty, the module disables
  itself.
- **Outside weather (Open-Meteo)** — the active location fetched over HTTPS
  once an hour, no API key, default `best_match` model. Temperature and
  apparent temperature, humidity, surface and sea-level pressure, UV index,
  cloud cover, wind speed / gusts / direction, precipitation and the WMO code,
  plus today's min/max from `daily`; `timezone=auto` yields
  `utc_offset_seconds` and the reply's `elevation` is kept.
  A request that never reached the API (DNS, TLS, timeout) is retried after
  15 s, then 30, 60, … up to 5 min; a reply that arrived but was unusable waits
  the full 5 min. The cached reading survives a failed fetch (age in
  `weather.age`) and is dropped once it passes an hour.
  The API carries the code, not the text: the device decodes it with
  `weather_api_code_str()` and the web UI with its own table (`wcode` in
  `index.html`) — the English wordings are identical and an edit to one belongs
  in the other. `weather_api_code_short()` is a third table, abbreviated to the
  10 characters the 16x2 panel leaves next to the outdoor temperature.
- **Weather locations** — up to 10 named `{name, lat, lon}` plus the active
  index in NVS. Empty on first boot: until one is added the card stays empty
  and no fetch is made. Switching wakes the fetch task via
  `weather_api_refresh()`.
- **SNTP** — UTC from `pool.ntp.org`; until the first sync `system.time` is
  dashes.

## Architecture

`app_main` (main.c) only initializes modules and exits; everything runs in
tasks and callbacks. Modules live under `main/`, each with a small public
header, grouped by what they face: `main/sensors/` talks to the I2C bus,
`main/ui/` drives the station's own hardware, `main/web/` serves HTTP
(including the page itself, `web/index.html`). Every subdirectory is in the
component's `INCLUDE_DIRS`, so includes stay flat (`#include "screen_16x2.h"`).
A quantity derived from one device's own readings and shown on that device's
card belongs in that device's module, not in the caller — dew point in
`scd40.c`, white/ALS ratio in `veml7700.c`.

| Module | Role |
|---|---|
| `wifi.c` | connection state machine; one snapshot from `wifi_get_info()` with separate STA and AP fields, plus the radio-free `wifi_sta_state()` the display polls |
| `wifi_store.c` | saved credentials in NVS (`wifi_creds`), mutex-protected |
| `web/webserver.c` | esp_http_server + mDNS; routes in one table, handlers through a wrapper that logs and blinks the LED; replies via a bounded appender that truncates rather than overrunning |
| `ota.c` | `POST /api/ota` + rollback confirmation; publishes `ota_is_active()` and `ota_progress_percent()` |
| `ui/led.c` | LED task: polls wifi/sensors/ota each tick, picks the pattern; persisted brightness |
| `button.c` | BOOT button: click → next page, 1.5 s hold → AP toggle |
| `sensors/sensors.c` | one task polling all four sensors at their own periods through a shared hot-plug state machine; owns the snapshots and the cross-sensor wiring (BMP581 pressure → SCD40 compensation) |
| `sensors/i2c_bus.c` | the I2C master bus and the recursive lock arbitrating it, for sensors and display alike |
| `sensors/i2c_dev.c` | shared register access: attach, probe, raw transfers, u8/u16 reads and writes |
| `sensors/scd40.c` | Sensirion command protocol with CRC-8, phased start, pressure compensation, FRC, dew point |
| `sensors/tmp117.c` | address auto-detection, device-ID check, config, temperature register |
| `sensors/bmp581.c` | address auto-detection, chip-ID check, soft reset out of deep standby, DSP/IIR + OSR/ODR setup, 6-byte burst read |
| `sensors/veml7700.c` | command registers, auto-ranging table with its settle deadline, lux conversion with the >1000 lx correction, white/ALS ratio |
| `sysinfo.c` | the station's own health: a boot-time snapshot of what cannot change plus live counters, the SoC temperature sensor, reset reason. CPU load is measured in one 1 s window shared by all callers |
| `ui/screen_16x2.c` | four button-advanced pages plus two conditional ones that pre-empt them, 10 fps loop, backlight dimmed to the ambient light. Sized for 16x2 |
| `ui/lcd1602_rgb.c` | DFR0464 transport: character output, backlight registers, revision detection, hot-plug recovery. The only file tied to this display |
| `timesync.c` | SNTP client; `timesync_is_synced()` and `timesync_format()` |
| `sensors/climate.c` | the room-level view over the devices, plus the reduction to sea level and the site-altitude setting. Its header carries the reading resolutions |
| `history.c` | three rings sampled from a 1 s esp_timer; the two longer ones persist to `/data` with a versioned header |
| `storage.c` | mounts the LittleFS `storage` partition at `/data` |
| `telegram.c` | message queue + sender task; `telegram_notify(fmt, ...)` |
| `weather_api.c` | Open-Meteo client; own task fetches the active location hourly, `weather_api_refresh()` forces a reload |
| `weather_store.c` | saved locations + active index in NVS (`weather_loc`), mutex-protected |
| `alerts.c` | notification rules and thresholds; own task polls every 10 s |
| `settings.c` | thin u8/u32/i32 get/set over the NVS namespace `settings` |

## HTTP API

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | embedded single-page UI (gzipped) |
| `/api/status` | GET | full status JSON in objects, nothing at the top level: `sta` / `ap`, `climate` (`temp`, `rh`, `co2`, `press`, `press_msl`, `lux` — a number or `null` with no sensor behind it), `sensors` (one object per device with its own `ok`, including what it derives — SCD40 `dew`, VEML7700 `white_ratio`), `weather`, `system`, `settings` (`led_brightness`, `backlight_rgb`, read-only `backlight_scale`, `altitude`) |
| `/api/history` | GET | `?p=5m\|1h\|1d` (default `1d`); `{period, co2, temp, rh, press, lux}`, each series gated on its own quantity so `null` is a gap in that series alone. `press` comes out reduced to sea level |
| `/api/history/reset` | POST | wipe all tiers, RAM rings and flash snapshots |
| `/api/scan` | GET | Wi-Fi scan, `[{ssid, bssid, ch, rssi, auth}]`, one entry per BSSID |
| `/api/networks` | GET / POST / DELETE | saved networks; POST `{"ssid", "password", "bssid"}` (bssid optional — pins to that AP), DELETE `{"ssid"}` |
| `/api/locations` | GET / POST / DELETE | saved locations; POST `{"name", "lat", "lon"}`, DELETE `{"index"}` |
| `/api/locations/active` | PUT | switch location; `{"index"}` (triggers an immediate refetch) |
| `/api/connect` | POST | leave AP mode / restart the STA connection cycle |
| `/api/settings` | POST | any subset of `{"led_brightness": 1–255, "backlight_rgb": "RRGGBB", "altitude": −500…9000}` |
| `/api/scd40/calibrate` | POST | forced recalibration; `{"ppm": 400–2000}`, returns the applied correction |
| `/api/ota` | POST | firmware update; raw binary body, `X-OTA-Key` header; reboots on success |

## Build & flash

```sh
./build.sh                     # build
./build.sh flash monitor       # first time (partition table changed) — by cable
./flash-ota.sh                 # afterwards — over the network
```

`build.sh` sources `$IDF_PATH/export.sh` (defaults to `~/esp/esp-idf`) and
forwards everything else to `idf.py`, so the environment does not have to be
set up in the shell first. Plain `idf.py …` still works in an exported shell.

Deliberate config deviations live in `sdkconfig.defaults`: 16 MB flash, custom
partition table, rollback, run-time stats for CPU load, `-Os`,
`LWIP_MAX_SOCKETS=16` (httpd + mDNS + SNTP must not starve the outbound TLS
clients) and a `1.1.1.1` DNS fallback for when the DHCP-supplied resolver
leaves a query unanswered. `sdkconfig` is generated and not tracked — delete it
after changing the defaults, or the old value wins.

`partitions.csv`: nvs 24K, otadata 8K, phy 4K, ota_0/ota_1 4M each, storage
(LittleFS) 6M pinned to the end of flash at 0xA00000 so future app-slot growth
does not move the data.
