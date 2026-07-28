# Weather Station

ESP32-C6 firmware (ESP-IDF 5.5, 16 MB flash) for a weather station. Work in
progress — networking, web UI and OTA infrastructure are done; all four I2C
sensors report into `/api/status`, the web page, the LCD and the history, and
the alerts are still the only consumer left tied to the SCD40 alone.

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
  (mDNS). Dark dashboard: five sensor cards (temperature / humidity / CO₂ on
  the first row, pressure / illuminance on the second) fed from the `climate`
  object, each with current value, trend arrow, min/max and a sparkline chart.
  The trend arrow shows the change across the whole displayed period (first
  sample of the window vs the latest one); min/max sit to the right of the big
  reading rather than on a row of their own, so the chart keeps that height
  — CO₂ carries a level badge instead of a trend arrow, and pressure carries a
  second line under the reading with the same value reduced to sea level.
  Then a
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
  system card (Wi-Fi details, chip temp, heap, CPU load, NVS as of boot, uptime, reset
  reason, firmware/build version), a settings card (LED
  brightness, LCD backlight color, site
  altitude in metres, SCD40 FRC calibration, and a history reset button that
  wipes all three rings — RAM and flash — after a confirm dialog) and a Wi-Fi
  card (one list — saved networks by
  default, augmented with scanned APs on Scan; saved and the connected AP are
  badged; tap an AP → password modal with an optional "pin BSSID" toggle →
  save; per-network delete; reconnect button). The
  altitude field saves on change and is written back from the device only
  once per page load, so a 1 s status poll cannot overwrite what is being
  typed.
  The backlight color is a single hex field (`00AAFF`, a leading `#` is fine)
  with a color dot next to it previewing the current value; what is typed is
  sent to the device verbatim, and a malformed value is rejected with the
  field put back to the color the device is showing.
  The header is a single row of equal-height controls — title, then the chart
  period switch, the language switch, the gear and the connection pill.
  The system/settings/Wi-Fi cards are hidden by default behind that gear toggle
  (state persists in localStorage), so the page opens as a
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
  the next 100 ms tick, with the bus free meanwhile. That retry is deliberately
  the tick and not the sensor's own period: a sequence of several steps would
  otherwise cost a full period each, which is what used to put seconds between
  a torch on the VEML7700 and the reading on the display.
  Sub-millisecond-to-few-ms waits inside the drivers are busy-waits
  (`esp_rom_delay_us`): the FreeRTOS tick is 10 ms, so
  `vTaskDelay(pdMS_TO_TICKS(n))` for a few ms rounds down to zero ticks and
  does not delay at all.
  Everything the rest of the firmware needs from these devices it takes
  through `climate.c`, one quantity at a time, rather than per chip — the
  exception is the alerts, which still read the SCD40 directly.
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
    the sample that triggered the change is dropped. A count at the 16-bit
    ceiling is exempt from that arithmetic — it is a lower bound, not a
    measurement, so the driver drops straight to the coarsest step (25 ms) and
    lets the table settle from there in one move. After any configuration
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
  - indoor + headline — `23.46° 45.3 1250` / `12.3° Overcast` (indoor
    temperature, humidity and CO₂ from `climate`; outdoor temperature and the
    decoded WMO description, cut to the room left). Sixteen characters take the
    standard resolution but not the `%` after the humidity — of the two markers
    on the row the degree sign is the one worth keeping, since the row below
    carries a temperature too. A five-digit CO₂ (past the SCD40's specified
    range, but not impossible) would still overrun, and the temperature drops
    to one decimal for as long as it does: a truncated number reads as a
    plausible wrong one
  - indoor precise — `23.46° 1013.250` / `1250 45.3% 999.9` (TMP117 °C and
    BMP581 hPa; below them SCD40 CO₂ and humidity plus VEML7700 illuminance).
    The unit suffixes are what the second row spends its characters on
    instead of digits. That leaves five columns for the illuminance, so its
    tenth of a lux survives below 1000 lx, then the row drops to whole lux and
    past 9999 lx to kilolux — the only reading in the firmware that ever shows
    fewer digits than the standard. Each value is rendered on its own, so one
    absent sensor shows as `--` and leaves the rest readable
  - outdoor detail — `12.3° 8..17°U3` / `78% 1013 NW15 3` (current temperature,
    today's min/max, UV index; humidity %, sea-level pressure hPa, wind as an
    8-point compass label + km/h, WMO code)

  - system — `17:27:40 28.07` / `12% 184k BL184` (time in the active
    location's timezone with the colons blinking on a two-second swing — one
    second lit, one second blank — day and month — the year does not fit alongside the seconds; below
    them CPU load over the last second, free heap in KiB and the backlight
    scale the illuminance is currently producing, which has no other readout
    on the device itself)

  The backlight color is stored as plain RGB (NVS `bl_rgb`, default `00AAFF`);
  the device knows nothing about hue or color models. What reaches the panel is
  that color scaled by the ambient light: the VEML7700 reading is mapped
  logarithmically — like the perception of brightness and like the four decades
  a room spans — from 1 lx or less, where the scale sits at its 16/255 floor
  (dim, never dark: a black screen reads as a fault), up to 100 lx and above,
  where the stored color goes out untouched. Because of the logarithm the top
  of that range is nearly flat, so the exact ceiling matters far less than the
  floor does. There is no setting: it is always on, and the one thing that
  turns it off is a missing reading — no VEML7700, or none yet, means full
  brightness. A channel that is lit stays lit at the bottom end (scaled to at
  least 1), otherwise it would drop out of the mix and shift the hue. The web
  page shows and edits the stored color, so at night it will read brighter than
  the panel looks; the scale itself is on the display's system page and in
  `settings.backlight_scale`. Recomputing costs a `log10f` — the C6 has no FPU
  — so the result is cached against the reading it came from: sixty frames a
  second ask, and the sensor answers once a second.
  Redrawn at 60 fps, paced by an `esp_timer` (the 10 ms FreeRTOS tick is too
  coarse for a 16.7 ms frame); that is the polling rate — the transport skips
  frames that would repaint identical characters, so the actual bus traffic
  follows how often the readings change. Missing data shows as
  `no sensor data` / `no weather data`. Hot-plug: an absent display is
  re-probed every 5 s. Text is ASCII-only — the character ROM has no Cyrillic,
  so other bytes show as `?`.
- **Climate** — the room as opposed to the chips. One source per quantity and
  **no fallbacks**: temperature is the TMP117's and nothing else, humidity and
  CO₂ the SCD40's, pressure the BMP581's, illuminance the VEML7700's. The
  SCD40 and the BMP581 also report a temperature and both are ignored — a
  sensor quietly standing in for a missing TMP117 would put a step of about a
  degree into the charts, and that step is indistinguishable from a real
  event. A quantity with no sensor behind it is reported as absent, never as
  zero. Pure composition of the published snapshots (`climate.c`): no task, no
  state, no lock. Exposed as the `climate` object in `/api/status`, and it is
  what the main cards and the history both read, so a card and the chart under
  it cannot disagree. Alongside the measured pressure it carries the same
  reading **reduced to sea level** — the international barometric formula, with
  the site altitude (a setting, see below) as its only input. That standard
  atmosphere is the whole approximation: the real air column has its own
  temperature, and in hard frost a temperature-corrected reduction lands up to
  ~2 hPa higher. Taking that temperature from the indoor sensor would be worse
  than assuming the standard profile — the column is outdoors — and taking it
  from the weather API would make a local reading stop working when the network
  does. At altitude 0 the factor is exactly 1, so an unconfigured station
  reports its measurement unchanged rather than something subtly wrong. The
  reduced value is not stored in the history: at a fixed altitude it is the
  measured pressure times a constant, so its curve would be the same one
  shifted.
- **Site altitude** — metres above sea level, −500…9000, stored in NVS
  (`settings/altitude_m`, default 0) and set from the settings card on the web
  page. Nothing else depends on it.
- **Reading resolution** — one per quantity, the same everywhere a value is
  shown or stored: the web page, the HTTP API, the charts, the history rings
  and the LCD. Written down once in `climate.h`, next to the quantities
  themselves.

  | Quantity | Resolution | Sensor | Format |
  |---|---|---|---|
  | temperature | 0.01 °C | TMP117 | `%.2f` |
  | pressure | 0.001 hPa | BMP581 | `%.3f` |
  | CO₂ | 1 ppm | SCD40 | `%u` |
  | humidity | 0.1 % | SCD40 | `%.1f` |
  | illuminance | 0.1 lx | VEML7700 | `%.1f` |

  These are resolutions, not accuracies — the parts are worth ±0.1 °C,
  ±0.3 hPa absolute (±0.06 hPa relative), ±(50 ppm + 5 %) and ±6 %RH. What
  they buy is that one reading reads identically in every place it appears,
  and that a five-minute chart window is not quantised into a single flat
  step. The 16x2 display is the sole exception, and only where sixteen
  characters run out — see its layouts below.
- **History & charts** — five quantities (CO₂, temperature, humidity,
  pressure, illuminance) in three ring buffers: 5 min of 1 s samples (RAM
  only), 1 h of 5 s points and 24 h of 1-min averages — 38 KB of rings plus a
  23 KB staging buffer the snapshots are copied through (it was 20 + 11 KB
  with three quantities at 8 bytes a point).
  Filled by **sampling** `climate_get()` once a second — nothing pushes into
  it, so each quantity keeps being recorded for exactly as long as its own
  sensor is alive, and validity is per quantity: an absent BMP581 leaves nulls
  in the pressure series alone. A sensor slower than the sampling rate (the
  SCD40 produces a result every 5 s) repeats its latest value into the 1 s
  tier; the averaged tiers are unaffected.
  Points are 16 bytes, at the reading resolutions below — fixed-point for the
  four bounded quantities and a float for illuminance, which spans six decades
  and fits no fixed scale.
  The two longer rings are snapshotted to LittleFS (`/data/hist_1h.bin` every
  5 min, `/data/history.bin` every 10 min, both on graceful shutdown / OTA
  reboot) and restored on boot — downtime shows up as a gap, anchored via SNTP
  time; points collected before the deferred restore are merged in after the
  gap. The snapshot header carries a version, and a file written by an older
  firmware is dropped rather than reinterpreted.
  The web UI draws a sparkline inside each of the five sensor cards with a
  small dependency-free canvas renderer: switchable window (5 min / hour /
  day, one switch for all charts, in the page header) fetched from
  `/api/history?p=` and re-polled
  at 2/10/60 s respectively, time/value axis labels, hover crosshair with a
  value + time tooltip; offline periods show as gaps. Value labels take their
  decimals from the gridline step, and the left margin follows the widest of
  them — `1013.250` does not fit the width `23.4` needs. The illuminance chart is
  drawn on a **logarithmic** y axis (gridlines 1/1.5/2/3/5/7 x 10^k, thinned
  as the window widens) — on a linear one everything below noon collapses onto
  the baseline. No charting library is bundled.
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
  dedicated "outside weather" card on the web page — both at the resolution
  Open-Meteo publishes: 0.1 for temperatures, pressures and wind, 0.01 for the
  UV index and precipitation, so nothing is rounded away on the way out;
  `weather_api_code_str()` decodes the WMO code to English text for the panel
  and the log. The API carries the code, not the text, so the web UI decodes it
  itself (`wcode` in `index.html`, one table per language) — its English table
  and `weather_api_code_str()` are the same wording word for word, and an edit
  to one belongs in the other.
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
tasks/callbacks. Modules under `main/`, each with a small public header, and
grouped by what they face:

- `main/sensors/` — everything that talks to a device on the I2C bus
- `main/ui/` — what the station shows on its own hardware: the status LED and
  the display
- `main/web/` — what it serves over HTTP, including the page itself
  (`web/index.html`)

Every subdirectory is in the component's `INCLUDE_DIRS`, so the includes stay
flat (`#include "screen_16x2.h"`, not `"ui/screen_16x2.h"`).

| Module | Role |
|---|---|
| `wifi.c` | connection state machine; STA state and AP state are separate fields of one snapshot from `wifi_get_info()` (polled by the LED task), with `wifi_is_connected()` as the shorthand and `wifi_ap_enable()` to toggle the AP; also names the link for the UI — `wifi_authmode_str()`, `wifi_sta_phy_str()` |
| `wifi_store.c` | saved credentials in NVS (namespace `wifi_creds`), mutex-protected |
| `web/webserver.c` | esp_http_server + mDNS; routes live in one table and all handlers go through a wrapper that logs the request (except `/api/status` — polled every 1 s) and blinks the LED; static buffers are safe (single httpd task) and replies are assembled through a bounded appender that truncates and logs instead of running past the buffer |
| `ota.c` | `POST /api/ota` handler + rollback confirmation |
| `ui/led.c` | LED task; polls wifi/sensors/ota each tick and picks the pattern, brightness (persisted) |
| `button.c` | BOOT button via espressif/button: click → next display page, 1.5 s hold → AP toggle |
| `sensors/sensors.c` | one task polling all four sensors, each at its own period, through a shared hot-plug state machine; owns the published snapshots and the cross-sensor wiring (SCD40 readings into `history.c`, BMP581 pressure into the SCD40's compensation) |
| `sensors/i2c_bus.c` | the I2C master bus and the recursive lock that arbitrates it, for sensors and display alike; `i2c_bus_scan()` logs every responding address, run once at startup |
| `sensors/i2c_dev.c` | the register access the drivers were each repeating: attach, probe, raw transfers, u8/u16 register reads and writes in both byte orders |
| `sensors/scd40.c` | SCD40 transport: Sensirion command protocol with CRC-8, the phased start sequence (stop → ASC check → run), pressure compensation, FRC |
| `sensors/tmp117.c` | TMP117 transport: address auto-detection (`0x48`–`0x4B`), device-ID check, config (continuous, 8 averaged samples, 125 ms cycle), temperature register |
| `sensors/bmp581.c` | BMP581/BMP580 transport: address auto-detection (`0x47`/`0x46`), chip-ID check, soft reset, DSP/IIR + OSR/ODR setup, 6-byte burst read |
| `sensors/veml7700.c` | VEML7700 transport: little-endian command registers, the auto-ranging gain / integration-time table with its settle deadline, lux conversion with the >1000 lx correction |
| `sysinfo.c` | how the station itself is doing, in two shapes: a snapshot of what cannot change until the next boot (firmware and IDF version, build stamp, chip revision, CPU clock, flash size, NVS fill) read once at start-up, and the live counters (uptime, CPU load, task count, four heap figures) sampled per call. Owns the SoC's own temperature sensor — not on the bus and measuring the die rather than the room, so it is reported with the health readings — the reset reason, and the start-up task-list dump. The CPU load is measured in one 1 s window shared by every caller, so the display and `/api/status` read the same figure and neither one's polling rate distorts the other's |
| `ui/screen_16x2.c` | what the display shows: three pages advanced by the button (selection persisted), 60 fps frame timer, backlight as a stored RGB value dimmed to the ambient light. Sized for 16x2 — a bigger display gets its own layout module |
| `ui/lcd1602_rgb.c` | transport for the DFR0464 panel: character output, backlight registers, revision detection, hot-plug recovery. Takes the bus and its lock from `i2c_bus.c` like the sensors do. The only file tied to this particular display |
| `timesync.c` | SNTP client; `timesync_is_synced()` flag and `timesync_format()` for the clock shown in the UI (dashes of the same shape until the clock is set) |
| `sensors/climate.c` | the room-level view over the devices: one source per quantity, no fallbacks, absent where there is no sensor. Composition of the snapshots from `sensors.c`, plus the reduction of the measured pressure to sea level; owns the site-altitude setting (cached from NVS — the reduction runs on every read). Its header also carries the firmware-wide reading resolutions |
| `history.c` | three RAM rings (5 min @ 1 s, 1 h @ 5 s, 24 h @ 1 min) of five quantities with per-quantity validity; samples `climate_get()` from a 1 s esp_timer — nothing pushes into it; the two longer rings persist to `/data/hist_1h.bin` / `/data/history.bin` (5/10-min snapshots + shutdown handler, versioned header) |
| `storage.c` | mounts the LittleFS `storage` partition at `/data` |
| `telegram.c` | message queue + sender task (Telegram Bot API over HTTPS); `telegram_notify(fmt, ...)` |
| `weather_api.c` | Open-Meteo client; own task fetches the active location's outdoor conditions (temp/feels-like, daily min/max, humidity, surface & MSL pressure, UVI, wind, clouds, precipitation, WMO code, elevation, UTC offset) over HTTPS once an hour, exposed via `weather_api_get()` / `weather_api_code_str()`; `weather_api_refresh()` forces an immediate reload on a location change |
| `weather_store.c` | saved weather locations `{name, lat, lon}` + active index in NVS (namespace `weather_loc`), mutex-protected; empty until the user adds a location |
| `alerts.c` | notification rules & thresholds; own task polls sensors/network every 10 s |
| `settings.c` | thin u8/u32/i32 get/set over NVS namespace `settings` |

## HTTP API

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | embedded single-page UI (index.html, gzipped) |
| `/api/status` | GET | full status JSON, grouped into objects: `sta` / `ap` (Wi-Fi), `climate`, `sensors`, `weather` (Open-Meteo), `system` (firmware, chip, heap, uptime, clock, plus `nvs_used`/`nvs_total` as counted once at boot — the count only moves when a setting is written, and walking the NVS pages on every poll is not worth it), `settings` (`led_brightness`, `backlight_rgb`, `backlight_scale` — read-only, what the ambient light is scaling that color by right now, 0-255 — and `altitude`); nothing is left at the top level. `climate` is the room — `temp`, `rh`, `co2`, `press`, `press_msl` (the same reading reduced to sea level from the configured altitude), `lux`, each a number or `null` when no sensor stands behind it. `sensors` is the hardware view: one object per I2C device, each with its own `ok` — `scd40` (`co2`, `temp`, `rh`), `tmp117` (`temp`), `bmp581` (`press` hPa, `press_pa`, `temp`), `veml7700` (`lux`, `white`, `als_raw`, `white_raw`, `gain`, `it`) — plus `chip_temp` (the SoC sensor, not on the bus) at the top of the group |
| `/api/history` | GET | `?p=5m\|1h\|1d` (default `1d`) selects the ring; returns `{period: <s>, co2: [...], temp: [...], rh: [...], press: [...], lux: [...]}`. Each series is gated on its own quantity, so `null` is a gap in that series alone |
| `/api/history/reset` | POST | wipe all history tiers (RAM rings and the `/data/hist_1h.bin` / `/data/history.bin` flash snapshots); sampling keeps running and refills from empty |
| `/api/scan` | GET | scan for Wi-Fi networks, returns `[{ssid, bssid, ch, rssi, auth}]` (one entry per BSSID — same SSID may repeat across APs) |
| `/api/networks` | GET | list of saved networks `[{ssid, bssid?}]` (bssid present only when the network is pinned to an AP) |
| `/api/networks` | POST | save a network; body `{"ssid": "...", "password": "...", "bssid": "aa:bb:.."}` (bssid optional — pins the connection to that AP; omit/all-zero = connect to strongest), updates existing SSID |
| `/api/networks` | DELETE | remove a saved network; body `{"ssid": "..."}` |
| `/api/locations` | GET | saved weather locations `{"active": <idx>, "locations": [{"name", "lat", "lon"}]}` |
| `/api/locations` | POST | add a location; body `{"name": "...", "lat": <n>, "lon": <n>}` (coordinates resolved in-browser via Open-Meteo geocoding), updates an existing name |
| `/api/locations` | DELETE | remove a location; body `{"index": <n>}` |
| `/api/locations/active` | PUT | switch the displayed location; body `{"index": <n>}` (triggers an immediate refetch) |
| `/api/connect` | POST | leave AP mode / restart the STA connection cycle |
| `/api/settings` | POST | apply settings; body `{"led_brightness": 1–255, "backlight_rgb": "RRGGBB", "altitude": −500…9000}` (hex string, leading `#` tolerated; altitude in metres above sea level), any subset, all persisted in NVS |
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
