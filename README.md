# Weather Station

ESP32-C6 firmware (ESP-IDF 5.5, 16 MB flash). Networking, web UI and OTA are
done; all four I2C sensors feed `/api/status`, the web page, the LCD and the
history.

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
  in-page dictionary. Five sensor cards (temperature, humidity, CO₂,
  pressure, illuminance) with trend, min/max and a sparkline, then two radar
  cards closing the same grid, each quantity read once: the presence card reads
  the live distance to the nearest target with the window's largest target count
  beside it, over a chart of distance as a line and target count as columns and
  under a badge carrying the live held state, and the radar card reads the
  number of targets with the nearest target's x/y in metres beside it and
  plots them instead of charting;
  a card per I2C device with a liveness dot; an outside-weather card with a location chip row (a typed
  city name is geocoded in-browser, so adding one needs internet on the
  client); system, settings and Wi-Fi cards behind the header gear. Settings:
  LED brightness, backlight color, site altitude, SCD40 FRC, history reset.
  Every reading is a fixed slot generated from the tables at the top of the
  script (`SERIES`, `WEATHER_TILES`, `SENSOR_DEVS`) and renders as `--` when
  absent — adding a metric is a table entry, not markup. `badge` replaces a card's
  trend arrow with a chip (CO₂ level, pressure tendency) and `footer` adds a
  line under its chart (the forecast wording). The altitude field is
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
- **mmWave radar (HLK-LD2450)** — UART1 at 256000 8N1, module TX on GPIO10,
  module RX on GPIO11, 5 V. The tracking stream is unprompted, so the only
  thing ever sent is one configuration sequence: **Bluetooth off**, which the
  module ships with on and which serves the same target stream to anyone in
  range with the vendor's app. Sent once — the module keeps the setting in its
  own flash and cannot be asked whether Bluetooth is on, so a flag in NVS
  (`settings/radar_bt_off`) is what spares every later boot the write and the
  module restart it ends with. Clear it by hand after replacing the module. A
  module that does not answer is left alone, logged, and retried next boot.
  A 30-byte frame every ~100 ms
  carries up to three **moving** targets as x/y/speed; coordinates are
  sign-magnitude, not two's complement. There is no length field or checksum, so
  framing is by header and tail alone and resync is a byte at a time. The driver
  publishes the targets with their distance, the distance to the nearest, and
  `presence` — the target count held for 5 s, because someone who stops moving
  drops out of the frame for seconds at a time. Silent for 2 s means offline.
  Logs under the tag `radar`, and only the stream's state, never the readings:
  coming up, and going silent — which names the pin and the baud rate, and is
  the wiring check.
  Shown on its own web card — a plan view of the fan with a dot per target —
  on its own LCD page, recorded in the history rings beside the climate
  quantities, and `presence` is what lights the panel.
- **Rotary encoder (DFRobot SEN0235, EC11)** — A on GPIO23, B on GPIO22, button
  on GPIO21; four wires, no supply. The module's own 47 kΩ pull-ups (R1–R3) are
  **desoldered** — without VCC they would drag the three lines through a
  floating rail — and the pull-ups in use are the chip's internal ones, which
  both PCNT and `iot_button` enable themselves. A/B are decoded by PCNT in full
  quadrature, four counts per detent: the hardware glitch filter tops out near
  12.8 µs on this chip while contact bounce lasts milliseconds, so what rejects
  bounce is the quadrature itself — a chattering contact counts +1/−1 around the
  detent and cancels out. A 10 ms timer converts counts to detents and files
  each one by direction *and* by whether the button was down; a frame drawn
  100 ms later could not work that out. The button is the same polled
  `iot_button` as BOOT, no interrupt; a turn with it held suppresses the click
  that would otherwise end the gesture. `encoder_take()` hands over everything
  since the last call and clears it — `cw`/`ccw`, `cw_held`/`ccw_held`, click,
  double-click and long-press counts, plus the live held state. No interaction
  scheme is implied: mapping turns to selection, editing or a menu belongs to
  the screens. On the LCD that scheme is the classic one: turning browses the
  pages either way, a 0.8 s hold opens the backlight screen and another leaves
  it, and a click inside it moves between the channels.
- **16x2 LCD** (DFRobot Gravity I2C LCD1602 RGB, DFR0464) — shares the sensor
  bus and its lock; controller at `0x3E`, backlight driver at whichever of
  `0x60` / `0x30` / `0x6B` / `0x2D` the board revision uses. Redrawn at 10 fps;
  an absent panel is re-probed every 5 s. Text is ASCII-only — the ROM has no
  Cyrillic and other bytes show as `?`, the exceptions being the degree sign
  (`\xDF`) and the solid block (`\xFF`). Seven pages, browsed by the encoder or
  advanced by a short BOOT click, remembered in NVS (`settings/screen_page`):

  | Page | Rows |
  |---|---|
  | indoor + headline | `23.46° 45% 1250` / `12.3°  Overcast` |
  | indoor precise | `23.46° 16.8 1250` / `1013.250  999.9x` |
  | radar | `Radar 2   1.85m` / `X-0.42  -1.42m/s` |
  | outdoor detail | `12.3° 8..17 C3` / `78% 1013 NW15 U3` |
  | zambretti | `1013.2 hPa v2.1` / `Chgable, rain` |
  | sun | `Day 05:25 20:59` / `Sun+32.4° v13:45` |
  | system | `17:27:40 28.07` / `12% 184k BL184` |

  Sixteen characters are the whole constraint: the humidity is whole percent on
  the headline page and gives up its slot to the dew point on the precise one,
  and the illuminance keeps a tenth of a lux only below
  1000 lx, then drops to whole lux and to kilolux — the one place in the
  firmware that shows fewer digits than the standard resolution.

  Two screens outside that rotation pre-empt whatever is selected; neither is
  stored, so the chosen page returns.
  - OTA update — `Updating...  67%` over a progress bar, one cell per 6.25 %.
    Stays at 100 % until the reboot, disappears on a failed upload. Reachable
    by nothing, and it blocks the knob and the button while it is up
  - Backlight — `Backlight 00AAFF` / `>R  0 G170 B255`. Opened and closed by a
    0.8 s hold of the encoder; a click steps `>` between R, G and B, turning
    moves that channel by 8 per detent and clamps at the ends, and the panel
    repaints as it goes — it is its own preview. NVS is written once, on the
    way out

  Wi-Fi state is reported by the LED only.

  The backlight color is stored as plain RGB (NVS `bl_rgb`, default `00AAFF`) —
  the device knows nothing about hue or color models: the encoder edits the
  three channels themselves. What reaches the panel is
  that color scaled by the ambient light, 0-255: mapped logarithmically from
  1 lx and below, where the scale sits at its floor of 10 (dim, never dark), up
  to 70 lx and above, where the color goes out untouched. A lit channel stays
  lit at the bottom end. No setting; a missing reading means full brightness.
  The scale eases into a new level a few units per frame instead of jumping, so
  a passing shadow does not flicker the panel.

  On top of that scale the radar gates the panel: lit while someone is in the
  fan, dark for an empty room, faded either way — up in 1.6 s, out in 6.4 s.
  The fade starts as soon as presence drops; the only hold is the radar's own
  5 s, which covers whoever stops moving. A silent or absent module leaves the
  panel lit.
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
  driver computes it. Follows the humidity. Not in the history: shown on the
  SCD40 card and on the precise LCD page in place of the humidity, and nowhere
  else.
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

- **History & charts** — five climate quantities and the radar in three rings:
  5 min of 1 s samples (RAM only), 1 h of 5 s points, 24 h of 1-min averages —
  38 KB plus a 23 KB staging buffer. Filled by **sampling** `climate_get()` and
  `ld2450_get()` once a second, so each quantity is recorded for as long as its
  own sensor lives and validity is per quantity. Points are 16 bytes:
  fixed-point for the bounded quantities, float for illuminance.
  The radar rides in the one byte the layout had spare, which is what keeps the
  point at 16: the **largest number of targets** the slot saw (2 bits) and the
  **mean distance to the nearest** of them (5 bits, a quarter of a metre per
  step, 0 for an empty fan). Max for the count and mean for the distance, each
  against its own failure — a person who stops moving drops out of the frame
  and would dent a mean count, one ghost frame would pull a minimum distance to
  the sensor. Occupancy is not stored: the share of a window that held anyone
  follows from the counts, and the page computes it.
  The two longer rings are snapshotted to LittleFS (`/data/hist_1h.bin` every
  5 min, `/data/history.bin` every 10 min, both on graceful shutdown / OTA
  reboot) and restored on boot; downtime shows as a gap anchored via SNTP time.
  The header is versioned and a file from older firmware is dropped.
  Charts are a dependency-free canvas sparkline per card: window 5 min / 1 h /
  1 d from `/api/history?p=`, re-polled at 2/10/60 s, with a hover crosshair
  and gaps for offline periods. Illuminance is drawn on a **logarithmic** y
  axis.
- **Zambretti forecast** — the station's own barometer only, no network: the
  Open-Meteo reading belongs to whichever location is selected, possibly
  another city.
  The **tendency** is a least-squares fit over the last 3 h of the 1 d ring —
  not the difference between its ends, which one outlier or one dropped slot
  would decide — graded 0 / ±1 / ±2 / ±3 at 0.5, 1.5 and 3.5 hPa per 3 h.
  It feeds the **Zambretti** ladder (sea-level pressure, tendency, and a winter
  step when the clock is set, hemisphere a build constant in `zambretti.c`) to
  one of 26 wordings, carried as a code A…Z with the texts in `zambretti.c`
  and,
  translated, in `zcode` in `index.html`. Absent until three hours are on
  record — 60 % of the slots filled and spread over at least two of them. Pure
  composition over the history, no task and no state beyond a 15 s cache.
  Shown on the Zambretti LCD page and on the pressure card, whose badge is the
  tendency (arrow repeated once per grade) and whose footer is the wording.
- **Sun** — sunrise, sunset, day length, the sun's angle above the horizon and
  the twilight band that angle falls in (day above +6°, golden hour down to the
  horizon, then civil / nautical / astronomical twilight at −6 / −12 / −18°,
  night below), from the active location's coordinates and the SNTP clock (NOAA
  sunrise equation, `sun.c`). No network: unlike the Open-Meteo reading beside it, it
  survives an outage and a reboot. Within a minute of the USNO tables in
  temperate latitudes, a few minutes past 60° where the sun grazes the horizon.
  Polar day and polar night are a state of their own, not missing data. Needs
  only a clock and a location: the times are absolute (unix UTC), and the
  offset — which a location has only after its first fetch — is what turns them
  into wall-clock time. Without it the angle, the day's length and the countdown
  still stand and only the two clock times read `--:--`. Shown as a line on the
  weather card and on its own LCD page, which counts down to the next crossing.
- **Telegram notifications** — push-only bot, two rules: the SCD40 CO₂ level
  crossing 800/1200/2000 ppm (±25 ppm hysteresis), and arrival/departure from
  the radar's presence flag, reported with how long the previous state lasted.
  A departure counts only after 5 min of confirmed absence, since the radar
  loses whoever sits still; an arrival after 3 s. Token and chat id are
  compile-time constants in `telegram.c`; left empty, the module disables
  itself.
- **Outside weather (Open-Meteo)** — the active location fetched over HTTPS
  every 15 min, no API key, default `best_match` model. Temperature and
  apparent temperature, humidity, surface and sea-level pressure, UV index,
  cloud cover, wind speed / gusts / direction, precipitation, daylight flag and
  the WMO code, plus today's min/max and sunshine / daylight duration from
  `daily` (the card shows the sunshine in hours and its share of the daylight);
  `timezone=auto` yields
  `utc_offset_seconds` and the reply's `elevation` is kept. `precipitation` is
  an accumulation over the model step, so it is kept only as a rate in mm/h,
  rescaled by the reply's `interval` (900 s on 15-minute models, 3600 s on
  hourly ones).
  A request that never reached the API (DNS, TLS, timeout) is retried after
  15 s, then 30, 60, … up to 5 min; a reply that arrived but was unusable waits
  the full 5 min. The cached reading survives a failed fetch (age in
  `weather.current.age`) and is dropped once it passes an hour, whether the
  fetch failed or the link never came up.
  The API carries the code, not the text: the device decodes it with
  `weather_api_code_str()` and the web UI with its own table (`wcode` in
  `index.html`) — the English wordings are identical and an edit to one belongs
  in the other. `weather_api_code_short()` is a third table, abbreviated to the
  10 characters the 16x2 panel leaves next to the outdoor temperature.
- **Weather locations** — up to 10 named `{name, lat, lon, utc_offset}` plus the
  active index in NVS. Empty on first boot: until one is added the card stays
  empty and no fetch is made. The first location added becomes the active one.
  Switching, deleting, and that first add wake the fetch task via
  `weather_api_refresh()`. The offset is filled from the first successful fetch
  and rewritten only when it changes, so the display clock is right after a
  reboot and through an outage, without a reading to read it from.
- **SNTP** — UTC from `pool.ntp.org`; until the first sync `system.time` is
  dashes.

## Architecture

`app_main` (main.c) only initializes modules and exits; everything runs in
tasks and callbacks. Modules live under `main/`, each with a small public
header, grouped by what they face: `main/sensors/` talks to the I2C bus,
`main/radar/` to the UART tracker,
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
| `led.c` | LED task: polls wifi/sensors/ota each tick, picks the pattern; persisted brightness |
| `button.c` | BOOT button: click → next page, 1.5 s hold → AP toggle |
| `encoder.c` | EC11 knob: PCNT quadrature behind a 10 ms poll, the button on `iot_button`; publishes raw detents and press events through `encoder_take()`, no interaction scheme of its own. Its header is esp-free so the GUI simulator can drive screens with it |
| `sensors/sensors.c` | one task polling all four sensors at their own periods through a shared hot-plug state machine; owns the snapshots and the cross-sensor wiring (BMP581 pressure → SCD40 compensation) |
| `sensors/i2c_bus.c` | the I2C master bus and the recursive lock arbitrating it, for sensors and display alike |
| `sensors/i2c_dev.c` | shared register access: attach, probe, raw transfers, u8/u16 reads and writes |
| `sensors/scd40.c` | Sensirion command protocol with CRC-8, phased start, pressure compensation, FRC, dew point |
| `sensors/tmp117.c` | address auto-detection, device-ID check, config, temperature register |
| `sensors/bmp581.c` | address auto-detection, chip-ID check, soft reset out of deep standby, DSP/IIR + OSR/ODR setup, 6-byte burst read |
| `radar/ld2450.c` | LD2450 on its own UART: reader task, frame resync and decode, the published snapshot with presence and the nearest target |
| `sensors/veml7700.c` | command registers, auto-ranging table with its settle deadline, lux conversion with the >1000 lx correction, white/ALS ratio |
| `sysinfo.c` | the station's own health: a boot-time snapshot of what cannot change plus live counters, the SoC temperature sensor, reset reason. CPU load is measured in one 1 s window shared by all callers |
| `display16x2/screen_16x2.c` | seven knob-browsed pages plus the backlight and OTA screens outside the rotation, the encoder drained once per frame, 10 fps loop, backlight dimmed to the ambient light and gated by radar presence. Sized for 16x2 |
| `display16x2/lcd1602_rgb.c` | DFR0464 transport: character output, backlight registers, revision detection, hot-plug recovery. The only file tied to this display |
| `gui/gfx/gfx_canvas.c` | drawing surface for the planned SSD1322 panel: a 64x256 portrait framebuffer already packed the way the controller wants it, a viewport stack carrying origin and clip, points, dashed h/v lines, rectangles |
| `gui/gfx/gfx_text.c` | text at a given level and alignment, baseline-positioned, optionally over a filled line box (`gfx_text_bg`). Drives u8g2's font decoder through its own `u8g2_cb_t`, so glyphs land in the canvas at the caller's gray level with no compositing pass |
| `gui/gfx/gfx_fonts.c` | generated: the u8g2 fonts actually linked, sliced by `gui/tools/extract_fonts.py` |
| `gui/ui_model.c` | one snapshot of everything a frame may read, taken before it starts drawing, so no reading changes mid-frame and no lock is held across one |
| `gui/ui.c` | the immediate-mode layer over the canvas: the text styles, a vertical layout cursor, separators |
| `gui/screens/screen_now.c` | the main screen — one function of the model, redrawn whole. Being built up element by element; currently the clock, the outdoor block (icon, temperature, conditions) and the pressure / humidity / UVI / wind rows |
| `timesync.c` | SNTP client; `timesync_is_synced()` and `timesync_format()` |
| `sensors/climate.c` | the room-level view over the devices, plus the reduction to sea level and the site-altitude setting. Its header carries the reading resolutions |
| `history.c` | three rings sampled from a 1 s esp_timer, climate and radar alike; the two longer ones persist to `/data` with a versioned header |
| `storage.c` | mounts the LittleFS `storage` partition at `/data` |
| `telegram.c` | message queue + sender task; `telegram_notify(fmt, ...)` |
| `weather_api.c` | Open-Meteo client; own task fetches the active location hourly, `weather_api_refresh()` forces a reload |
| `weather_store.c` | saved locations + their UTC offset + active index in NVS (`weather_loc`), mutex-protected |
| `sun.c` | sunrise/sunset/elevation for the active location; `sun_next_event()` is the countdown the display shows |
| `zambretti.c` | 3 h barometric tendency fitted over the 1 d ring, and the Zambretti wording it selects with the sea-level pressure |
| `alerts.c` | notification rules and thresholds; own task ticks every 1 s, air every 10 s |
| `settings.c` | thin u8/u32/i32 get/set over the NVS namespace `settings` |

## HTTP API

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | embedded single-page UI (gzipped) |
| `/api/status` | GET | full status JSON in objects, nothing at the top level: `sta` / `ap`, `climate` (`temp`, `rh`, `co2`, `press`, `press_msl`, `lux` — a number or `null` with no sensor behind it), `sensors` (one object per device with its own `ok`, including what it derives — SCD40 `dew`, VEML7700 `white_ratio`), `zambretti` (`trend` −3…+3, `delta_3h`, `code` 0…25 for A…Z; `null` until three hours of pressure are recorded), `sun` (`state` `rises`/`polar_day`/`polar_night`, `rise` / `set` as unix UTC or `null`, `day_len`, `up`, `elev`, `phase` `day`/`golden`/`civil`/`nautical`/`astro`/`night`, `next_in` / `next_is_rise` — seconds to the next crossing, counted on the device so a wrong browser clock cannot skew it; `null` without a clock or an active location), `radar` (`presence`, `near` — metres to the closest target — and `targets`, `x` / `y` in mm, up to three, plotted by the page; `null` while the LD2450 is silent), `weather` (two independently nullable halves: `loc` — `name`, `active`, `lat`, `lon`, `utc_offset` — known as soon as a location is saved, and `current`, the fetched reading with its `age`), `system`, `settings` (`led_brightness`, `backlight_rgb`, read-only `backlight_scale`, `altitude`) |
| `/api/history` | GET | `?p=5m\|1h\|1d` (default `1d`); `{period, co2, temp, rh, press, lux, targets, near}`, each series gated on its own quantity so `null` is a gap in that series alone. `press` comes out reduced to sea level; `targets` is the slot's largest target count, `near` metres in quarter-metre steps and `null` for a slot with nobody in the fan |
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

The drawing framework and the screens above it also build on the host, which is
how fonts and layouts are judged without the panel — `ui_model.c` stays out of
that build (it reads the sensor modules), so the harness fills a `ui_model_t`
by hand:

```sh
cd main/gui/sim && make run   # self-tests, the width table, then out/*.png at 4x
./sim 1                       # 1:1, the size the panel really is

python3 main/gui/tools/extract_fonts.py   # after editing FONTS in that script
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
