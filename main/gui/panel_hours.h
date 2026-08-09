#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How long the OLED has actually been lit, kept across boots -- the panel wears
 * by the hour it spends driving pixels, not by the station's uptime, and the
 * presence blanking means the two are far apart.
 *
 * Two counters: the plain lit seconds, and the same time weighted by the drive
 * current, i.e. seconds equivalent at full brightness. The datasheet rates the
 * panel at 50,000 hours to half brightness (25 C, 50 % checkerboard) and says
 * nothing about how the drive setting scales that, so the weighted figure is a
 * relative meter, not a prediction. */

void panel_hours_init(void);

/* Called once a frame by the gui task, which is the only writer. */
void panel_hours_track(bool lit, uint8_t bright);

void panel_hours_get(uint32_t *on_s, uint32_t *dose_s);
