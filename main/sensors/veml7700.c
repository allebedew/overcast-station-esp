#include "veml7700.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "i2c_dev.h"

#define VEML7700_ADDR 0x10 /* fixed, no address pin */

/* Command codes. Unlike the other devices on this bus the payload is
 * little-endian: low byte first. */
#define VEML7700_CMD_ALS_CONF     0x00
#define VEML7700_CMD_POWER_SAVING 0x03
#define VEML7700_CMD_ALS          0x04
#define VEML7700_CMD_WHITE        0x05

/* ALS_CONF_0: gain bits 12:11, integration time bits 9:6, shutdown bit 0. */
#define VEML7700_GAIN_SHIFT 11
#define VEML7700_IT_SHIFT   6
#define VEML7700_SHUTDOWN   0x0001

/* Gain field codes; the encoding is not monotonic — 1/8 and 1/4 sit above x1. */
#define VEML7700_GAIN_X1   0x0
#define VEML7700_GAIN_X2   0x1
#define VEML7700_GAIN_D8   0x2 /* x1/8 */
#define VEML7700_GAIN_D4   0x3 /* x1/4 */

/* Integration time field codes. */
#define VEML7700_IT_25   0xC
#define VEML7700_IT_50   0x8
#define VEML7700_IT_100  0x0
#define VEML7700_IT_200  0x1
#define VEML7700_IT_400  0x2
#define VEML7700_IT_800  0x3

/* Auto-ranging steps, least to most sensitive. Resolution is the datasheet
 * reference point (x2, 800 ms = 0.0036 lx/count) scaled by the ratios. */
typedef struct {
    uint8_t gain_code;
    uint8_t it_code;
    const char *gain_str;
    uint16_t it_ms;
    float lx_per_count;
} veml7700_range_t;

static const veml7700_range_t RANGES[] = {
    { VEML7700_GAIN_D8, VEML7700_IT_25,  "1/8", 25,  1.8432f },
    { VEML7700_GAIN_D8, VEML7700_IT_50,  "1/8", 50,  0.9216f },
    { VEML7700_GAIN_D8, VEML7700_IT_100, "1/8", 100, 0.4608f },
    { VEML7700_GAIN_D4, VEML7700_IT_100, "1/4", 100, 0.2304f },
    { VEML7700_GAIN_X1, VEML7700_IT_100, "1",   100, 0.0576f },
    { VEML7700_GAIN_X1, VEML7700_IT_200, "1",   200, 0.0288f },
    { VEML7700_GAIN_X2, VEML7700_IT_200, "2",   200, 0.0144f },
    { VEML7700_GAIN_X2, VEML7700_IT_400, "2",   400, 0.0072f },
    { VEML7700_GAIN_X2, VEML7700_IT_800, "2",   800, 0.0036f },
};
#define RANGE_COUNT ((int)(sizeof(RANGES) / sizeof(RANGES[0])))

/* Full daylight and a lit room both settle within a step or two of here. */
#define RANGE_START 4

/* Counts below/above which the range is one step off. The upper bound stays
 * clear of the 16-bit ceiling so a rising level cannot saturate unnoticed. */
#define RANGE_LOW_COUNTS  100
#define RANGE_HIGH_COUNTS 10000

/* At the 16-bit ceiling the count is a lower bound, not a measurement. */
#define RANGE_SATURATED 65535

/* Two integrations — the one under way when the write lands is discarded —
 * plus the datasheet's 2.5 ms power-on. */
#define VEML7700_SETTLE_MS(it_ms) ((int64_t)(it_ms) * 2 + 10)

static const char *TAG = "veml7700";

static i2c_master_dev_handle_t s_dev;
static int s_range = RANGE_START;

/* Earliest unread sample (µs, esp_timer clock): one integration after the last
 * reading, two after a configuration change. A poll arriving before it is
 * turned away rather than re-reading a published sample. */
static int64_t s_ready_at_us;

/* Where start() gave up, so the warning below can name the step. */
static const char *s_step = "";

static esp_err_t apply_range(int idx)
{
    const veml7700_range_t *r = &RANGES[idx];
    uint16_t conf = ((uint16_t)r->gain_code << VEML7700_GAIN_SHIFT) |
                    ((uint16_t)r->it_code << VEML7700_IT_SHIFT);
    /* the datasheet asks for a shutdown/power-up cycle around a config change */
    esp_err_t err = i2c_dev_write_u16le(s_dev, VEML7700_CMD_ALS_CONF,
                                        conf | VEML7700_SHUTDOWN);
    if (err == ESP_OK) {
        err = i2c_dev_write_u16le(s_dev, VEML7700_CMD_ALS_CONF, conf);
    }
    if (err == ESP_OK) {
        s_range = idx;
        s_ready_at_us = esp_timer_get_time() + VEML7700_SETTLE_MS(r->it_ms) * 1000;
    }
    return err;
}

/* Finest-resolution step the level still fits in. Working from the level means
 * a reading far outside the current range moves straight to the right step.
 * Saturation is the caller's problem — a clipped count lands several steps
 * short here. */
static int pick_range(uint16_t als)
{
    float lx = als * RANGES[s_range].lx_per_count;

    for (int i = RANGE_COUNT - 1; i > 0; i--) {
        if (lx / RANGES[i].lx_per_count <= RANGE_HIGH_COUNTS) {
            return i;
        }
    }
    return 0;
}

/* Vishay VISHAY-84323: above ~1000 lx the low-gain / short-integration steps
 * compress and need this fourth-order correction. */
static float correct_lux(float lux)
{
    if (lux <= 1000.0f) {
        return lux;
    }
    float x = lux;
    return ((6.0135e-13f * x - 9.3924e-9f) * x + 8.1488e-5f) * x * x + 1.0023f * x;
}

esp_err_t veml7700_start(void)
{
    s_step = "probe";
    if (!i2c_dev_present(VEML7700_ADDR)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_dev) {
        s_step = "attach";
        esp_err_t err = i2c_dev_attach(&s_dev, VEML7700_ADDR);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", VEML7700_ADDR,
                     s_step, esp_err_to_name(err));
            return err;
        }
    }

    /* No ID register: a foreign device at 0x10 passes as a VEML7700. */
    s_step = "power_saving";
    esp_err_t err = i2c_dev_write_u16le(s_dev, VEML7700_CMD_POWER_SAVING, 0);
    if (err == ESP_OK) {
        s_step = "range";
        err = apply_range(RANGE_START);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", VEML7700_ADDR,
                 s_step, esp_err_to_name(err));
        return err;
    }

    s_step = "";
    ESP_LOGI(TAG, "VEML7700 at 0x%02X, gain x%s / %u ms", VEML7700_ADDR,
             RANGES[RANGE_START].gain_str, RANGES[RANGE_START].it_ms);
    return ESP_OK;
}

esp_err_t veml7700_read(veml7700_data_t *out)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Reading early returns the published sample again, or after a config
     * change the previous settings' leftovers — near zero at power-up, which
     * reads as darkness and sends the driver climbing the table. */
    int64_t now = esp_timer_get_time();
    if (now < s_ready_at_us) {
        return ESP_ERR_NOT_FINISHED;
    }

    uint16_t als, white;
    esp_err_t err = i2c_dev_read_u16le(s_dev, VEML7700_CMD_ALS, &als);
    if (err == ESP_OK) {
        err = i2c_dev_read_u16le(s_dev, VEML7700_CMD_WHITE, &white);
    }
    if (err != ESP_OK) {
        return err;
    }

    /* Re-range and drop this sample: it was taken with the old settings and
     * integration restarts. The caller retries after the settle deadline. */
    if (als > RANGE_HIGH_COUNTS || als < RANGE_LOW_COUNTS) {
        /* A clipped count says only "at least this bright", so estimating from
         * it lands short and clips again. The coarsest step converges in one
         * change, at 25 ms of settling. */
        int want = als >= RANGE_SATURATED ? 0 : pick_range(als);
        if (want != s_range) {
            err = apply_range(want);
            return err == ESP_OK ? ESP_ERR_NOT_FINISHED : err;
        }
    }

    const veml7700_range_t *r = &RANGES[s_range];
    s_ready_at_us = now + (int64_t)r->it_ms * 1000;
    out->als_raw = als;
    out->white_raw = white;
    out->lux = correct_lux(als * r->lx_per_count);
    out->white_lux = correct_lux(white * r->lx_per_count);
    out->white_ratio = out->lux > 0.0f ? out->white_lux / out->lux : 0.0f;
    out->gain = r->gain_str;
    out->it_ms = r->it_ms;
    return ESP_OK;
}
