#pragma once

#include <stdbool.h>

/* Transport for the 256x64 SSD1322 panel (NHD-5.5-25664UCG3), 4-wire SPI.
 *
 * The panel is mounted rotated, so everything above gfx_target.h works in
 * portrait; the only place that knows about the physical orientation is the
 * re-map byte in the init sequence. This module is the implementation of
 * gfx_present() — nothing else should talk to the panel. */

/* Brings up the SPI bus, resets the panel and runs the init sequence. */
void ssd1322_init(void);

/* Test patterns, wired to nothing above: 0 turns every pixel on through a single
 * command (no RAM involved), 1 fills RAM instead. Together they separate a dead
 * command path from a dead data path. Orientation and mirroring are settled by
 * drawing a scene instead — screen_test is where that goes. */
void ssd1322_selftest(int pattern);
