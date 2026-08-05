# Archive

Code that no longer builds and is kept only so it can be brought back. Nothing
here is on any include path — ESP-IDF scans `components/` and `main/`, not this
directory — so it cannot drift into a build by accident. It will not compile
as-is either: the APIs around it keep moving.

## display16x2

The DFRobot Gravity I2C LCD1602 RGB (DFR0464), removed when the SSD1322 panel
took over. `lcd1602_rgb.c` is the transport, `screen_16x2.c` the nine pages and
the backlight.

Two things in `screen_16x2.c` outlive the panel itself and are the reason this
is kept rather than deleted:

- **Ambient dimming** — the VEML7700 reading mapped logarithmically to a 0-255
  scale, floor 10 at 1 lx and below, untouched at 70 lx and above, eased a few
  units per frame so a passing shadow does not flicker anything.
- **Presence gating** — the panel lit while the radar sees someone and dark for
  an empty room, faded up in 1.6 s and out in 6.4 s.

Both want re-implementing for the OLED, where the knob is the SSD1322 contrast
register rather than an RGB backlight.

To bring the display back: the sources go to `main/display16x2/`, then
`main/CMakeLists.txt` (`SRCS` and `INCLUDE_DIRS`), `screen_16x2_init()` in
`main.c`, the BOOT single-click handler in `button.c`, and `backlight_rgb` /
`backlight_scale` in `web/webserver.c` and `web/index.html`. NVS still holds its
`bl_rgb` and `screen_page` keys — they were never cleared.
