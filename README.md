# Weather Station

ESP32-C6 firmware (ESP-IDF 5.5, 16 MB flash). Networking, web UI and OTA are
done; all four I2C sensors feed `/api/status`, the web page, the panel and the
history.

## Implemented

- **Wi-Fi (STA)** — up to 5 networks in NVS, tried in order, 5 attempts each
  5 s apart. Every attempt scans all channels and joins the strongest BSSID for
  the SSID unless the network is pinned to one. All failing (or an empty store)
  falls back to AP mode.
- **AP mode** — `WeatherStation` / `weather123`. Runs as APSTA on the STA
  channel, so an existing router association survives and the forecast and
  Telegram keep using it while the AP is up. `sta` and `ap` are
  independent objects in `/api/status`. BOOT (GPIO9): 1.5 s hold toggles
  AP ↔ STA; the short click is unused. Leaving AP mode keeps a surviving
  association and only switches the mode back — reconnection happens when
  the store changes (`/api/connect`).
- **Web UI** — single page embedded in the firmware (gzipped at build time),
  `http://weather.local` (mDNS), polled every 1 s, bilingual RU/EN from an
  in-page dictionary. Five sensor cards (temperature, humidity, CO₂,
  pressure, illuminance) with trend, min/max, a sparkline and a liveness dot
  for the device behind the quantity, then two radar
  cards closing the same grid, each quantity read once: the presence card reads
  the live distance to the nearest target with the window's largest target count
  beside it, over a chart of distance as a line and target count as columns and
  under a badge carrying the live held state, and the radar card reads the
  number of targets with the nearest target's x/y in metres beside it and
  plots them instead of charting;
  an outside-weather card with a location chip row (a typed
  city name is geocoded in-browser, so adding one needs internet on the
  client); system, settings and Wi-Fi cards behind the header gear. Settings:
  LED brightness, buzzer volume, display on, its auto-brightness and its
  brightness, radar
  Bluetooth, site altitude, SCD40 FRC, history reset. Every setting
  follows the device on each poll except while the control has the focus, so
  what the knob changes shows up here without overwriting a moving hand.
  Every reading is a fixed slot generated from the tables at the top of the
  script (`SERIES`, `WEATHER_TILES`) and renders as `--` when
  absent — adding a metric is a table entry, not markup. `badge` replaces a card's
  trend arrow with a chip (CO₂ level, pressure tendency) and `footer` adds a
  line under its chart — wording on the left, an optional value on the right
  edge (the other thermometers, the dew point, the measured pressure, the
  white/lux ratio with the VEML7700's gain and integration time, the forecast
  wording). The altitude field is
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
- **Buzzer** (passive piezo KPT-1410 on GPIO19, straight on the pin) — one LEDC
  channel, seventeen tunes, a new one cuts off whatever is playing: three clicks
  at different pitches, two bare tones well below resonance (2.5 and 1.5 kHz,
  there to be listened to), `OK`/`WARN`/`ALARM`/`ERROR`, one per CO₂ zone edge in
  each direction (the motif repeats once per zone crossed, rising going up and
  falling coming down), `STORM` and the boot chirp. The
  element resonates at 4 kHz and drops off
  steeply either side, so tunes differ by rhythm inside 3.5–4.5 kHz, not by
  pitch. Volume is the PWM duty, 1–50 % of full swing (the fundamental goes as
  sin(π·duty), so 50 % is the loudest the element gets), set on the buzzer page
  and kept in NVS (`settings/buzz_vol`). The pin is pulled down and left alone
  for 10 ms before LEDC claims it — driving 10 nF straight to ground clicks.
  The three clicks are wired to the encoder on the panel, a CO₂ zone edge plays
  its tune from `alerts.c`, and a fetch that
  lands ticks once — pitched high or low by where the outdoor temperature went
  since the previous fetch, plain when it held; the layer that arbitrates a UI click against an
  alarm is still to come — `buzzer_play()` cuts off whatever is playing.
- **I2C sensors** — SDA GPIO2 / SCL GPIO3, all of it in `main/sensors/`: the
  bus, one transport file per device, and a single task ticking at 10 ms that
  brings each sensor up at its own period through one hot-plug state machine —
  probe until it answers, then read; 3 consecutive failures drop it back to
  probing every 5 s. Setup failures are diagnosed from the log: each driver
  names the address, the step it gave up on and the I2C error. Going offline —
  whether the device answered once and stopped or was never there — is one
  error line per disappearance, not one per probe, so an empty bus says so at
  boot and then keeps quiet.
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
  - **AS3935** (`0x03`, jumpered; 0x01/0x02 in reserve) — lightning. The IRQ
    pin is **not wired**, so the latched interrupt flag is polled at **20 Hz**:
    strike timestamps are only as good as that period, two strikes inside one
    collapse into one, and the LCO antenna tuning is impossible — the resonance
    frequency comes out on the IRQ pin only, so `TUN_CAP` stays 0 until that
    pin is temporarily wired. Gain is **indoor** (`AFE_GB` 18), `MIN_NUM_LIGH`
    1 (accumulation hides an isolated distant storm) and `MASK_DIST` 0. Only
    `NF_LEV` adapts: a step up on every noise-too-high report, a step back down
    after 10 quiet minutes; `WDTH`/`SREJ` stay at 2/2 because they cut real
    sensitivity silently. The RCO calibration runs at every start. The distance
    register never expires, so an hour without a strike drops the storm state
    and clears the part's statistics. Logs a line per strike (distance,
    energy), per noise-floor move and a disturber count per minute; a detection
    also plays `BUZZER_STORM`. Not yet on the web page, the panel or in the
    history, and none of it is a setting yet.
- **mmWave radar (HLK-LD2450)** — UART1 at 256000 8N1, module TX on GPIO10,
  module RX on GPIO11, 5 V. The tracking stream is unprompted, so the only
  thing ever sent is one configuration sequence: **Bluetooth on or off**, which
  the module ships with on and which serves the same target stream to anyone in
  range with the vendor's app. `settings/radar_bt_off` (default off) is the
  state the module is meant to hold, and the reader task carries a change into
  it — on its own task, because the UART is the one it is draining. Nothing is
  written at boot: the module keeps Bluetooth in its own flash and cannot be
  asked about it, so the setting is the only account of it there is, and
  writing it back every boot would cost a module restart each time. A module
  swapped under a station that already has the setting is the case this gets
  wrong; flipping the setting twice sorts it out. One attempt per change — a
  module that does not answer is logged, and flipping again is the retry. A
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
  recorded in the history rings beside the climate quantities, and `presence`
  is what used to light the panel.
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
  the screens. Drained by the GUI's render task, one `encoder_take()` per frame.
- **256x64 OLED** (Newhaven NHD-5.5-25664UCG3, SSD1322) — 4-wire SPI on SPI2 at
  8 MHz, mounted rotated so everything above the transport works in portrait
  64x256. A render task at **10 frames/s** refreshes the model, folds in the
  encoder and draws; the frame is then compared with the 8 KB copy of what the
  panel is showing and flushed only if a pixel differs — `gfx_present` takes a
  fixed 8.3 ms of SPI whatever changed (DMA, with the task blocked rather than
  spinning), and the comparison is at pixel level because a reading that moves
  below its displayed resolution changes the model without changing the screen.
  Render times are logged every 10 min, and only while frames are actually being
  flushed.
  Switching the panel on and off is a **0.4 s dissolve**: the lit pixels of the
  frame are added, and removed, in random order, and the frame period drops to
  20 ms while it runs. The controller's display-on comes first and display-off
  last, so the hardware is only driving pixels while there is something to see;
  OTA takes the panel over without one.
  Against burn-in the whole frame is drawn 0–5 rows lower than its layout puts
  it, one row every 10 min, walking the range and turning round at each end;
  only a lit panel advances it, and OTA draws at 0. The shift clips at the
  bottom rather than wrapping, so screens keep `GFX_SHIFT_MAX` rows free there.
  How far that has to go is metered by `gui/panel_hours.c`: lit seconds, and the
  same time weighted by the drive current as seconds at full brightness, both in
  `nvs/panel/hours` (one `u64`, written every 15 min of lit time, when the panel
  goes dark and on shutdown) and reported under `system.panel`. The datasheet
  rates the panel at 50,000 h to half brightness at 25 °C and 50 % pixels on, and
  says nothing about how the drive setting scales that, so the weighted figure
  ranks brightness settings against each other rather than predicting hours.
  **Brightness** is the master current (`0xC7`), 16 steps; it is one of the
  knob's fields, kept in `settings/disp_bright` (default 12) and shown as 0–15
  in the bottom-right corner, prefixed by `A ` in auto mode and `- ` in manual.
  **Auto** (`settings/disp_auto`, default on, the knob's step past level 15)
  drives the level off the VEML7700 instead: full at 200 lx and up, by decade
  below that, the dimmest step at 1 lx and under. It follows the reading on the
  frame it arrives on, with a 0.6-step dead band so a room resting between two
  steps does not flicker. The panel walks to a new level a step a frame rather
  than jumping — a second and a half for the whole range, a frame for a knob
  detent. The manual level is kept while auto runs, and the corner
  always shows the level the panel is actually driven at. The steps are not evenly spaced —
  0 → 1 doubles the current, 14 → 15 adds 7 %. Everything else that scales the
  same current is fixed in the init sequence: contrast (`0xC1`) at 255, which
  makes it the ceiling the current scales down from, and the pre-charge — phase 2
  at 9 clocks (`0xB1`), voltage 5 (`0xBB`), second period 1 (`0xB6`). Newhaven's
  own pre-charge values light the pixel outside the phase the drive current
  controls, and their floor was bright enough that no contrast or current setting
  could dim the panel. The second pre-charge period must stay ≥ 1: a 0 there does
  not shorten the phase, it jumps the panel to bright.
  None of those is persisted or reachable from the panel: what a tuning session
  settles on is written into `gfx_target.h`, which the init sequence reads.
  Experiment stage above that: `screen_now`, plus `screen_ota` which takes the
  panel over while a firmware image is being received — it ignores the knob and
  lights a panel that was switched off. The only moving parts
  are the signal bar sweeping while the link is being made, three dots with one
  lit walking the same way in place of the reading's age while a fetch runs, the
  sun bar's centre reading swapping every 5 s, and the Zambretti wording
  scrolling back and forth when it is wider than the row left it. The
  knob drives its chart — **turning back** walks the four fields (weather
  location, which quantity, which window, brightness) and an empty focus that
  picks nothing and carries no plate, **turning forward**
  cycles the picked
  field's value, all wrapping; the brightness field's ring is the 16 levels
  followed by auto. A turn forward moves nothing only on the empty
  focus and on the location field with fewer than two saved. A **click**
  switches the panel off
  (`0xAE`, and nothing is rendered or clocked out while it is dark) and another
  brings it back; the state is kept in `settings/disp_on`, so a panel switched
  off stays dark across a reboot. The panel is lit only when that state and the
  radar's presence flag agree — the flag's own 5 s hold is what keeps the panel
  from flickering, so it needs none of its own; a silent radar counts as
  present, so a dead module cannot blank the panel. Every change of that lit
  state plays `CLICK`, suppressed when the knob is what caused it, since the
  click for the press has already sounded. Both it and the brightness are also readable
  and settable over the HTTP API, which goes through the settings module and
  never calls into the render task; that task compares the two settings with
  what it last saw at the top of every frame and adopts whatever moved, so
  panel commands stay on one task. A click is stored as it happens, the other
  fields once the turn has settled for 2 s — one NVS write per detent is
  what the delay avoids. The chart's quantity and window are persisted the same
  way (`settings/chart_q`, `settings/chart_range`, off the HTTP API), so the
  panel comes back to the chart it was left on. The location field works the same way: the name in the
  status bar follows the knob at once, and the pick reaches
  `weather_store_set_active()` — which persists it and refetches — 2 s after the
  turn stops. A location added, removed or selected over the web is adopted on
  the next frame and drops any pending pick.
  The selection is marked by a `GFX_HL` plate: on the
  quantity field it sits under the indoor reading being plotted and moves with
  it, on the window field under the badge below the chart, on brightness under
  the number in the bottom-right corner, on the location under the name in the
  status bar, which goes full level so as not to sink into the plate. The buzzer tells a
  field move from a value move, and the selection is logged on change. Both gestures land in `ui_state_input()`, and
  what they move — the panel's power, the focus, the two chart fields, the
  brightness and its mode, the selected location — is all of
  `ui_state_t`: the screen draws from it and holds no state of its own.

  | Panel pin | To |
  |---|---|
  | 1, 5 (VSS) | GND, both wires of the supply cable |
  | 2 (VDD) | 3.3 V, logic only |
  | 3 (BC_VDD) | 5 V, boost converter — 220 mA typ at 100 % pixels |
  | 4 (D/C) | GPIO4 |
  | 7 (SCLK) | GPIO6 (IOMUX FSPICLK) |
  | 8 (SDIN) | GPIO7 (IOMUX FSPID) |
  | 16 (/RES) | GPIO5 |
  | 17 (/CS) | GND — the display is alone on the bus |

  Module jumpers: **R3, R5, R8 closed, R2, R4, R6, R9, R10 open**. R3/R5 pull
  BS0/BS1 low for 4-wire SPI; R8 with R9 removed is the datasheet's Jumper
  Option #1, which splits the supplies — the logic then draws 280 µA from 3.3 V
  instead of the 350 mA the single-supply default pulls through the on-board
  boost converter. Pin 18 (BC_CTRL) enables that converter and is left floating:
  it measures 29 kΩ to ground rather than to VDD, which reads like a pull-down,
  but the converter does come up on its own. A dark panel means a wire from 18
  to 2 before anything else is suspected.

  /CS on ground costs the only thing that re-syncs the controller's bit counter,
  so /RES is pulsed at init and is the way back from a bus glitch; SSD1322 has
  no software reset.
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
  driver computes it. Follows the humidity. Not in the history: shown in the
  humidity card's footer, and nowhere else.
- **Site altitude** — metres above sea level, −500…9000, NVS
  `settings/altitude_m` (default 0). A wrong altitude shifts every pressure
  readout and chart, never the stored history.
- **Reading resolution** — one per quantity, the same in the web page, the API,
  the charts, the rings and the panel. Written down in `climate.h`. These are
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
  reboot) and restored as soon as the clock can be trusted, which anchors the
  downtime gap; unsynced past 5 min they are joined on with a single hole
  instead. The clock counts as trusted when SNTP answers, or at boot when the
  RTC — which survives everything but a power cycle — still reads a plausible
  date.
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
  Shown on the pressure card, whose badge is the
  tendency (arrow repeated once per grade) and whose footer is the wording, and
  on the panel's main screen as the 3 h change followed by the shortened
  wording, clipped to the row.
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
  weather card, which counts down to the next crossing.
- **Telegram notifications** — push-only bot: a comfort band crossed in either
  direction (every band in `alert_rules.c`, the same ones the panel blinks off;
  each rule arms on its own first reading), rain forecast for the next hour
  (≥70 %, re-armed under 50 %), an I2C sensor silent for 5 min and its return,
  one line at boot with the reset reason, and arrival/departure from the
  radar's presence flag, reported with how long the previous state lasted.
  A departure counts only after 5 min of confirmed absence, since the radar
  loses whoever sits still; an arrival after 3 s. Token and chat id are
  compile-time constants in `telegram.c`; left empty, the module disables
  itself.
- **Outside weather (Open-Meteo)** — the active location fetched over HTTPS
  every 15 min on the quarter-hour, no API key, `icon_seamless` model
  (`WEATHER_API_MODEL` in `weather_api.c`). Temperature and
  apparent temperature, humidity, surface and sea-level pressure, UV index
  (derived outside the models, so a named model returns it as null — a second
  minimal request without `models` fills it in, and the card shows `--.-` if
  that one fails too),
  cloud cover, wind speed / gusts / direction, precipitation, daylight flag and
  the WMO code, plus today's daylight duration from `daily`. `daily` also
  carries a seven-day forecast — date, min/max temperature, precipitation
  probability, mean cloud cover and WMO code per day, index 0 being today, which
  is where today's min/max comes from. `hourly` adds precipitation probability
  for the next 24 hours, `forecast_hours` making the series start at the current
  hour rather than at local midnight; the first hour's stamp is stored with it,
  so the display can re-index the series as the reading ages.
  `timeformat=unixtime` reports GMT+0, so the stored date is
  shifted by the location's offset and read back with `gmtime_r()`.
  `timezone=auto` yields `utc_offset_seconds` and the reply's `elevation` is kept. `precipitation` is
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
  10 characters a row leaves next to the outdoor temperature.
- **Weather locations** — up to 10 named `{name, lat, lon, utc_offset}` plus the
  active index in NVS. Empty on first boot: until one is added the card stays
  empty and no fetch is made. The first location added becomes the active one.
  Switching, deleting, and that first add wake the fetch task via
  `weather_api_refresh()`. The offset is filled from the first successful fetch
  and rewritten only when it changes, so the display clock is right after a
  reboot and through an outage, without a reading to read it from.
- **SNTP** — UTC from `pool.ntp.org`, started on `IP_EVENT_STA_GOT_IP` and
  restarted on every reconnect, so neither a boot nor an outage pays the lwIP
  backoff. Until the first sync `system.time` is dashes.

## Architecture

`app_main` (main.c) only initializes modules and exits; everything runs in
tasks and callbacks. Modules live under `main/`, each with a small public
header, grouped by what they face: `main/sensors/` talks to the I2C bus,
`main/radar/` to the UART tracker,
`main/gui/` drives the panel, `main/web/` serves HTTP (including the page
itself, `web/index.html`). Every subdirectory is in the component's
`INCLUDE_DIRS`, so includes stay flat (`#include "i2c_bus.h"`). `archive/` at
the repo root is outside all of it — code kept only so it can be brought back,
never scanned by the build; see its own README.
A quantity derived from one device's own readings and shown on that device's
card belongs in that device's module, not in the caller — dew point in
`scd40.c`, white/ALS ratio in `veml7700.c`.

| Module | Role |
|---|---|
| `wifi.c` | connection state machine; one snapshot from `wifi_get_info()` with separate STA and AP fields, plus the radio-free `wifi_sta_state()` the display polls |
| `wifi_store.c` | saved credentials in NVS (`wifi_creds`), mutex-protected |
| `web/webserver.c` | esp_http_server + mDNS; routes in one table, handlers through a wrapper that logs and blinks the LED; replies via a bounded appender that truncates rather than overrunning |
| `ota.c` | `POST /api/ota` + rollback confirmation; publishes `ota_is_active()` and the byte counts `ota_get_progress()` |
| `led.c` | LED task: polls wifi/sensors/ota each tick, picks the pattern; persisted brightness |
| `buzzer.c` | passive piezo on one LEDC channel: the tune table and a task that plays it, waiting out each note on its request queue so a new tune preempts mid-note |
| `button.c` | BOOT button: click → next page, 1.5 s hold → AP toggle |
| `encoder.c` | EC11 knob: PCNT quadrature behind a 10 ms poll, the button on `iot_button`; publishes raw detents and press events through `encoder_take()`, no interaction scheme of its own. Its header is esp-free so the GUI simulator can drive screens with it |
| `sensors/sensors.c` | one task polling all four sensors at their own periods through a shared hot-plug state machine; owns the snapshots and the cross-sensor wiring (BMP581 pressure → SCD40 compensation) |
| `sensors/i2c_bus.c` | the I2C master bus and the recursive lock arbitrating it |
| `sensors/i2c_dev.c` | shared register access: attach, probe, raw transfers, u8/u16 reads and writes |
| `sensors/scd40.c` | Sensirion command protocol with CRC-8, phased start, pressure compensation, FRC, dew point |
| `sensors/tmp117.c` | address auto-detection, device-ID check, config, temperature register |
| `sensors/bmp581.c` | address auto-detection, chip-ID check, soft reset out of deep standby, DSP/IIR + OSR/ODR setup, 6-byte burst read |
| `radar/ld2450.c` | LD2450 on its own UART: reader task, frame resync and decode, the published snapshot with presence and the nearest target |
| `sensors/veml7700.c` | command registers, auto-ranging table with its settle deadline, lux conversion with the >1000 lx correction, white/ALS ratio |
| `sysinfo.c` | the station's own health: a boot-time snapshot of what cannot change plus live counters, the SoC temperature sensor, reset reason. CPU load is measured in one 1 s window shared by all callers; a background task logs the per-task CPU share every 30 s |
| `gui/gfx/ssd1322.c` | the panel's transport and the only implementation of `gfx_target.h`: SPI setup, reset, the datasheet's init sequence, and a present that is one 8 KB DMA write because the canvas is packed the way the controller scans. The only file tied to this display |
| `gui/gfx/gfx_canvas.c` | drawing surface for the SSD1322 panel: a 64x256 portrait framebuffer already packed the way the controller wants it, a viewport stack carrying origin and clip, points, dashed h/v lines, rectangles |
| `gui/gfx/gfx_text.c` | text at a given level and alignment, baseline-positioned, optionally over a filled line box (`gfx_text_bg`). Drives u8g2's font decoder through its own `u8g2_cb_t`, so glyphs land in the canvas at the caller's gray level with no compositing pass |
| `gui/gfx/gfx_fonts.c` | generated: the u8g2 fonts actually linked, sliced by `gui/tools/extract_fonts.py` |
| `gui/views/ui_model.c` | one snapshot of everything a frame may read, taken before it starts drawing, so no reading changes mid-frame and no lock is held across one |
| `gui/views/ui.c` | the immediate-mode layer over the canvas: the text styles, a vertical layout cursor, separators, the illuminance format the room's row and the chart's axis share |
| `gui/views/chart.c` | the chart as one element: the plot and the labelled row under it. A widget — the series and which quantity it is are passed in, so it neither samples the history nor knows how either was picked. Carries the per-quantity axis: label format, minimum span, and whether the columns go linearly or by decade, and the `CHART_RANGES` table of windows |
| `gui/views/ui_state.c` | what the UI remembers about itself and the whole encoder scheme: the click switches the panel off, a turn back walks the knob's fields, a turn forward cycles the value of the one in focus. The weather location is the first of those fields, the chart's quantity and window the next two and the panel brightness the last (its ring being the 16 levels then auto, which follows the light sensor), with an empty focus ahead of them that the state boots on, which is why the selection lives here and not under the screen. They are `ui_settings_t`, exactly what is kept in NVS: the GUI task reads the struct in at boot and writes it back once a turn has settled. The location is held as an index into the store, whose count the GUI task feeds in, so the state machine stays free of esp headers |
| `gui/gui_loop.c` | the panel as the rest of the firmware sees it: owns the single 8 KB canvas, brings up the transport, draws a frame, and applies what the knob moved — the panel settings and the active location — once the turn has settled. The only firmware-only file in `gui/` |
| `gui/panel_hours.c` | the panel's own wear meter: lit seconds and brightness-weighted seconds, fed one frame at a time by the GUI task, packed into a single `u64` in the NVS namespace `panel` so a power cut cannot split them |
| `gui/views/screen_now.c` | the main screen — one function of the model, redrawn whole. Being built up element by element; live so far are the status bar (weekday, local time of the weather location, Wi-Fi bars, sweeping while an association attempt is on the air and replaced by an inverted `AP` badge while the SoftAP is up, location name, age of the fetch) the outdoor block (a 16x16 icon for the WMO code — sun, moon after sunset, cloud, drizzle, rain, snow, thunderstorm, fog, or a mark for an unknown code; temperature; conditions), the rows under it (feels-like, sea-level pressure, humidity, wind with gusts, UV index, cloud cover), the 24-hour rain strip (one column an hour from the current one, grouped by a wider gap at midnight, 06:00, noon and 18:00 local; every hour a 3 px column, its brightness rising from dim2 to full with the precipitation probability, a dry one and an hour without a forecast left at dim), the seven-day forecast (weekday letter, dim and a shade brighter at the weekend; min/max with a striped bar over the whole forecast's range; precipitation probability in tens, its brightness rising with its value; mean cloud cover as a one-pixel-wide column whose height (1, 3 or 5 px) and brightness (1..15) both rise with the clear sky, so a sunny day is a tall bright column) and the indoor rows (temperature, sea-level pressure, humidity, CO2, illuminance, dew point). A value outside its comfort band blinks, its label and plate holding their place — 1 Hz one zone out, 2 Hz two or more; the bands themselves are `alert_rules.c` and the severity comes ready-made in the model. Illuminance is the one value shown at less than its stored resolution: tenths below 1 lx, whole lux to 1000, thousands above. The battery is drawn empty — the board has no charge source. Below them a chart of one history quantity, 60 columns, with the ends of its scale and the window it covers labelled under it. The scale is the series' own min..max but never narrower than a per-quantity minimum set at the sensors' own noise (0.2 °C, 1 %RH, 0.3 hPa, 50 ppm, one decade of lux), so a flat hour reads as flat; illuminance is placed by decade, the rest linearly. Neighbouring points are joined by a riser and the corner column at each end of a flat run is moved one row toward the level it heads for, so a sensor's own quantisation draws as a slope rather than a staircase. Which quantity and which window are the knob's two fields, held in `ui_state_t` and handed both to `ui_model_refresh()`, so it knows what to sample, and to `chart_draw()`; the windows themselves are the `CHART_RANGES` table in `chart.c` — 1m, 5m, 1h, 1d, each 60 columns off the tier whose slots divide into it. Below the chart the Zambretti block, and under it the sun bar: a checkered body for the whole day with the daylight stretch a shade brighter, a tick every six hours, a pointer over the current hour, sunrise and sunset under its ends, and between them the wait for the next crossing swapping every 5 s with the sun's elevation. All of it from `sun.c` by way of the model, no forecast involved; a polar day fills the bar and a polar night leaves it empty |
| `gui/views/screen_ota.c` | the update screen: title, progress bar with the percentage inside it — drawn twice and split at the edge of the fill, so a digit the fill runs into inverts mid-glyph — and the byte counts read from `ota.c`. Drawn by the GUI task instead of everything else while `ota_is_active()` |
| `timesync.c` | SNTP client; `timesync_is_synced()` and `timesync_format()` |
| `sensors/climate.c` | the room-level view over the devices, plus the reduction to sea level, the dew-point spread (TMP117 temperature less the SCD40's dew point — cross-device, so neither driver owns it) and the site-altitude setting. Its header carries the reading resolutions |
| `history.c` | three rings sampled from a 1 s esp_timer, climate and radar alike; the two longer ones persist to `/data` with a versioned header. `history_series()` decimates one quantity out of a tier for the panel's chart, averaging each column over the slots it covers, in display units and NAN for a gap |
| `storage.c` | mounts the LittleFS `storage` partition at `/data` |
| `telegram.c` | message queue + sender task; `telegram_notify(fmt, ...)` |
| `weather_api.c` | Open-Meteo client; own task fetches the active location on the quarter-hour, `weather_api_refresh()` forces a reload |
| `weather_store.c` | saved locations + their UTC offset + active index in NVS (`weather_loc`), mutex-protected; a change that makes the shown reading stale calls the `weather_store_on_change()` hook, which `weather_api.c` registers, so an edit refetches without its caller asking for it |
| `sun.c` | sunrise/sunset/elevation for the active location; `sun_next_event()` is the countdown the display shows |
| `zambretti.c` | 3 h barometric tendency fitted over the 1 d ring, and the Zambretti wording it selects with the sea-level pressure |
| `alert_rules.c` | the comfort bands, shared by the panel's blinking readings and the Telegram alerts: a ladder of zones per quantity plus the comfortable one, so a two-sided band, a ceiling and a floor are one shape. Indoor CO2 800/1200/2000 ppm, temperature 22/30 °C, humidity 20/80 %, dew-point spread 3/1 °C; outdoor UV index 8, temperature −10/0/30 °C, gusts 50/70 km/h, pressure tendency ±1.6/±3 hPa/3 h; each with a hysteresis margin set at its source's noise. `alert_sample()` is the one place that says which reading feeds which rule. Numbers only, no state, no wording |
| `alerts.c` | what a crossing is said and sounded like: Telegram on every quantity in `alert_rules.c`, on a confirmed arrival or departure, on a sensor gone silent for 5 min and back, on rain forecast for the next hour, and once at boot with the reset reason; plus the CO2 buzzer tune — one motif per zone crossed, up or down, and always the three-motif one on landing above 2000. Own task ticks every 1 s, the rest every 10 s |
| `settings.c` | every persistent scalar setting in one descriptor table (NVS key, API key, label, default, range) over the NVS namespace `settings`; values cached in RAM at `settings_init()`, `settings_set()` clamps to the range, writes only on a change and then calls the owning module's `settings_on_change()` hook, so a caller stores a setting without knowing who holds it. Hooks run on the caller's task and touch RAM only — the panel polls its two settings instead |

## HTTP API

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | embedded single-page UI (gzipped) |
| `/api/status` | GET | full status JSON in objects, nothing at the top level: `sta` / `ap`, `climate` (`temp`, `rh`, `co2`, `press`, `press_msl`, `lux` — a number or `null` with no sensor behind it), `sensors` (one object per device with its own `ok`, including what it derives — SCD40 `dew`, VEML7700 `white_ratio`), `zambretti` (`trend` −3…+3, `delta_3h`, `code` 0…25 for A…Z; `null` until three hours of pressure are recorded), `sun` (`state` `rises`/`polar_day`/`polar_night`, `rise` / `set` as unix UTC or `null`, `day_len`, `up`, `elev`, `phase` `day`/`golden`/`civil`/`nautical`/`astro`/`night`, `next_in` / `next_is_rise` — seconds to the next crossing, counted on the device so a wrong browser clock cannot skew it; `null` without a clock or an active location), `radar` (`presence`, `near` — metres to the closest target — and `targets`, `x` / `y` in mm, up to three, plotted by the page; `null` while the LD2450 is silent), `weather` (two independently nullable halves: `loc` — `name`, `active`, `lat`, `lon`, `utc_offset` — known as soon as a location is saved, and `current`, the fetched reading with its `age`), `system` (uptime, build, heap, NVS, plus `panel` — `on_s` / `dose_s`, the OLED's lit and brightness-weighted seconds), `settings` (generated from the settings table: `led_brightness`, `buzzer_volume`, `display_on`, `display_brightness`, `display_auto_brightness`, `radar_bt_off`, `altitude`) |
| `/api/history` | GET | `?p=5m\|1h\|1d` (default `1d`); `{period, co2, temp, rh, press, lux, targets, near}`, each series gated on its own quantity so `null` is a gap in that series alone. `press` comes out reduced to sea level; `targets` is the slot's largest target count, `near` metres in quarter-metre steps and `null` for a slot with nobody in the fan |
| `/api/history/reset` | POST | wipe all tiers, RAM rings and flash snapshots |
| `/api/scan` | GET | Wi-Fi scan, `[{ssid, bssid, ch, rssi, auth}]`, one entry per BSSID |
| `/api/networks` | GET / POST / DELETE | saved networks; POST `{"ssid", "password", "bssid"}` (bssid optional — pins to that AP), DELETE `{"ssid"}` |
| `/api/locations` | GET / POST / DELETE | saved locations; POST `{"name", "lat", "lon"}`, DELETE `{"index"}` |
| `/api/locations/active` | PUT | switch location; `{"index"}` (triggers an immediate refetch) |
| `/api/connect` | POST | leave AP mode / restart the STA connection cycle |
| `/api/settings` | POST | any subset of the keys `/api/status` reports under `settings`, driven by the same table: `led_brightness` 1–255, `buzzer_volume` 1–50, `display_on` bool, `display_brightness` 0–15, `display_auto_brightness` bool, `radar_bt_off` bool, `altitude` −500…9000. Out of range is clamped; an unknown key or a wrong type is a 400 naming it |
| `/api/scd40/calibrate` | POST | forced recalibration; `{"ppm": 400–2000}`, returns the applied correction |
| `/api/ota` | POST | firmware update; raw binary body, `X-OTA-Key` header; reboots on success |

## Build & flash

```sh
./build.sh                     # build
./build.sh flash monitor       # first time (partition table changed) — by cable
./flash-ota.sh                 # afterwards — over the network
```

The same commands are Zed tasks (`esp: …` in `.zed/tasks.json`); the OTA task
always targets the default `weather.local`.

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
