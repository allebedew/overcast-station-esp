#include "ui_model.h"

#include <string.h>

void ui_model_refresh(ui_model_t *out)
{
    memset(out, 0, sizeof(*out));

    out->now       = time(NULL);
    out->utc_off_s = weather_api_utc_offset_s();

    climate_get(&out->climate);
    out->out_ok   = weather_api_get(&out->out);
    out->out_cond = weather_api_code_short(out->out.weather_code);
}
