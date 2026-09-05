#include "as3935.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "i2c_dev.h"

/* Set by the module's ADD0/ADD1 jumpers; 0x01 and 0x02 are the alternatives if
 * anything ever collides. */
#define AS3935_ADDR 0x03

/* Module IRQ -> GPIO0. Not a strapping pin on this chip, and the 32 kHz
 * crystal it doubles as is unused (the RTC runs off the internal RC). */
#define AS3935_IRQ_GPIO 0

#define AS3935_REG_AFE       0x00 /* bits 5:1 AFE_GB, bit 0 PWD */
#define AS3935_REG_NF        0x01 /* bits 6:4 NF_LEV, bits 3:0 WDTH */
#define AS3935_REG_STAT      0x02 /* bit 6 CL_STAT, 5:4 MIN_NUM_LIGH, 3:0 SREJ */
#define AS3935_REG_INT       0x03 /* 7:6 LCO_FDIV, 5:4 MASK_DIST, 3:0 INT */
#define AS3935_REG_ENERGY    0x04 /* 0x04-0x06, little-endian, 21 bits */
#define AS3935_REG_DISTANCE  0x07 /* bits 5:0 */
#define AS3935_REG_TUN       0x08 /* 7 DISP_LCO, 6 DISP_SRCO, 5 DISP_TRCO, 3:0 TUN_CAP */
#define AS3935_REG_CALIB_TRCO 0x3A /* bit 7 done, bit 6 failed */
#define AS3935_REG_CALIB_SRCO 0x3B
#define AS3935_REG_PRESET    0x3C
#define AS3935_REG_CALIB_RCO 0x3D
#define AS3935_DIRECT_CMD    0x96 /* the only value 0x3C and 0x3D accept */

#define AS3935_TUN_DISP_LCO  0x80
#define AS3935_TUN_DISP_TRCO 0x20

#define AS3935_INT_MASK      0x0F
#define AS3935_INT_NOISE     0x01
#define AS3935_INT_DISTURBER 0x04
#define AS3935_INT_LIGHTNING 0x08

/* Indoor gain. Outdoor is 14, and getting this wrong is fatal either way:
 * indoor gain outdoors saturates on everything, outdoor gain indoors hears
 * nothing. It becomes a setting once the sensor's final home is known. */
#define AS3935_AFE_GB_INDOOR 18

/* Noise floor: where it starts, and how far the adaptation may push it. */
#define AS3935_NF_LEV_DEFAULT 2
#define AS3935_NF_LEV_MAX     7

/* Watchdog threshold and spike rejection: where they start and how far the
 * disturber loop below may push them. Both buy immunity with sensitivity, so
 * they move only against a measured disturber rate and always walk back. */
#define AS3935_WDTH_DEFAULT 2
#define AS3935_WDTH_MAX     6
#define AS3935_SREJ_DEFAULT 2
#define AS3935_SREJ_MAX     6

/* Disturbers per minute that make the loop tighten, and the rate it takes to
 * let it loosen again after the quiet period. */
#define AS3935_DISTURBERS_HIGH 30
#define AS3935_DISTURBERS_LOW  5
#define AS3935_REJECT_DECAY_MS (10 * 60 * 1000)

/* Reserved bit 7 reads 1; CL_STAT 1, MIN_NUM_LIGH 0 (= one strike, no
 * accumulation — accumulation hides an isolated distant storm). SREJ occupies
 * the low nibble. */
#define AS3935_STAT_BASE 0xC0
#define AS3935_STAT_CL_STAT 0x40

/* Datasheet: 2 ms for the direct commands, for the TRCO settling pulse, and
 * between an event and the read of the interrupt register. Busy-waited where
 * it has to be — the FreeRTOS tick is 10 ms, so a short vTaskDelay is a no-op. */
#define AS3935_SETTLE_US 2500

/* Quiet before the noise floor may step back down. Long, because the point of
 * lowering it is sensitivity, not a fast reaction. */
#define AS3935_NF_DECAY_MS (10 * 60 * 1000)

/* Nothing in the part expires by itself — the distance register holds its last
 * estimate forever — so both spans are the driver's. An hour without a strike
 * clears the part's accumulated statistics; a day without one drops the record
 * of the last strike entirely. The same hour is the bucket the 24 h count is
 * kept in, which is why it is also its resolution. */
#define AS3935_HOUR_MS       (60 * 60 * 1000)
#define AS3935_STRIKE_HOURS  24

#define AS3935_DISTURBER_WINDOW_MS 60000

/* Antenna tuning. DISP_LCO puts the tank's frequency divided by LCO_FDIV (16
 * at the default) on the IRQ pin, where a counter can measure it; the tank is
 * 500 kHz nominal and the datasheet allows it ±3.5 %. The gate is long enough
 * for ~1250 counts, so the measurement itself resolves ~0.1 %. */
#define AS3935_LCO_NOMINAL_HZ   31250
#define AS3935_LCO_TOLERANCE_HZ 1094
#define AS3935_LCO_GATE_US      40000
#define AS3935_TUN_CAP_MAX      15
#define AS3935_LCO_GLITCH_NS    1000 /* one period is 32 us; this is pickup only */
#define AS3935_LCO_COUNT_LIMIT  20000

static const char *TAG = "as3935";

static i2c_master_dev_handle_t s_dev;

static uint8_t s_nf_lev = AS3935_NF_LEV_DEFAULT;
static uint8_t s_wdth = AS3935_WDTH_DEFAULT;
static uint8_t s_srej = AS3935_SREJ_DEFAULT;
static uint32_t s_strikes;
static uint8_t s_distance = AS3935_DISTANCE_OUT_OF_RANGE;
static uint32_t s_energy;

static int64_t s_last_strike_us;   /* 0 = none since start */
static int64_t s_last_noise_us;
static bool s_stats_dirty;         /* the part has statistics worth clearing */

/* Strikes per whole hour, oldest to newest around s_hour; their sum is the
 * 24 h count, so the oldest hour in it is a partial one. */
static uint16_t s_hours[AS3935_STRIKE_HOURS];
static int s_hour;
static int64_t s_hour_us;

/* Disturbers are counted over a window and published as the rate of the last
 * completed one, so the number does not sag to zero between events. */
static int s_disturbers;
static uint16_t s_disturbers_min;
static int64_t s_window_us;
static int64_t s_last_busy_us; /* end of the last window that was noisy */

/* Result of the last antenna sweep. */
static uint8_t s_tun_cap;
static uint16_t s_lco_hz;

/* The IRQ pin is a level: it goes high on an event and stays high until the
 * interrupt register is read. The handler only timestamps the edge — the read
 * itself belongs on the bus in the sensors task. */
static portMUX_TYPE s_irq_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t s_irq_us;
static volatile bool s_irq_pending;
static bool s_isr_attached;

/* Where start() gave up, so the warning below can name the step. */
static const char *s_step = "";

static esp_err_t write_nf(uint8_t nf_lev, uint8_t wdth)
{
    return i2c_dev_write_u8(s_dev, AS3935_REG_NF, (nf_lev << 4) | wdth);
}

static esp_err_t write_srej(uint8_t srej)
{
    return i2c_dev_write_u8(s_dev, AS3935_REG_STAT, AS3935_STAT_BASE | srej);
}

/* CL_STAT is a level, not a command: it clears the accumulated statistics on a
 * high-low-high transition. */
static esp_err_t clear_statistics(void)
{
    esp_err_t err = i2c_dev_write_u8(s_dev, AS3935_REG_STAT,
                                     (AS3935_STAT_BASE & ~AS3935_STAT_CL_STAT) | s_srej);
    if (err == ESP_OK) {
        err = write_srej(s_srej);
    }
    return err;
}

/* ---------------- interrupt line ---------------- */

static void IRAM_ATTR irq_isr(void *arg)
{
    s_irq_us = esp_timer_get_time();
    s_irq_pending = true;
}

/* Input with a pull-down: the part drives the line both ways, but an
 * unpopulated module then reads as "no event" instead of floating. */
static esp_err_t irq_gpio_configure(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << AS3935_IRQ_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    return gpio_config(&cfg);
}

static esp_err_t irq_attach(void)
{
    if (s_isr_attached) {
        return gpio_intr_enable(AS3935_IRQ_GPIO);
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* someone else's already */
        return err;
    }
    err = gpio_isr_handler_add(AS3935_IRQ_GPIO, irq_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }
    s_isr_attached = true;
    return gpio_intr_enable(AS3935_IRQ_GPIO);
}

/* ---------------- antenna tuning ---------------- */

static pcnt_unit_handle_t s_pcnt;
static pcnt_channel_handle_t s_pcnt_chan;
static int s_sweep_cap = -1; /* < 0 = no sweep in progress */
static int64_t s_sweep_us;
static int s_best_cap;
static int s_best_err;
static uint16_t s_best_hz;

static void lco_counter_free(void)
{
    if (!s_pcnt) {
        return;
    }
    pcnt_unit_stop(s_pcnt);
    pcnt_unit_disable(s_pcnt);
    /* The channel holds a reference to the unit: deleting it first is what
     * makes the unit free rather than leak on the next start. */
    if (s_pcnt_chan) {
        pcnt_del_channel(s_pcnt_chan);
    }
    pcnt_del_unit(s_pcnt);
    s_pcnt_chan = NULL;
    s_pcnt = NULL;
    s_sweep_cap = -1;
    /* The counter took the pin over through the GPIO matrix; give it back. */
    irq_gpio_configure();
}

static esp_err_t lco_counter_init(void)
{
    pcnt_unit_config_t unit_cfg = {
        .low_limit = -1,
        .high_limit = AS3935_LCO_COUNT_LIMIT,
    };
    esp_err_t err = pcnt_new_unit(&unit_cfg, &s_pcnt);
    if (err != ESP_OK) {
        return err;
    }

    pcnt_glitch_filter_config_t filter = { .max_glitch_ns = AS3935_LCO_GLITCH_NS };
    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = AS3935_IRQ_GPIO,
        .level_gpio_num = -1,
    };
    err = pcnt_unit_set_glitch_filter(s_pcnt, &filter);
    if (err == ESP_OK) {
        err = pcnt_new_channel(s_pcnt, &chan_cfg, &s_pcnt_chan);
    }
    if (err == ESP_OK) {
        err = pcnt_channel_set_edge_action(s_pcnt_chan,
                                           PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                           PCNT_CHANNEL_EDGE_ACTION_HOLD);
    }
    if (err == ESP_OK) {
        err = pcnt_unit_enable(s_pcnt);
    }
    if (err == ESP_OK) {
        err = pcnt_unit_start(s_pcnt);
    }
    if (err != ESP_OK) {
        lco_counter_free();
    }
    return err;
}

/* Routes the tank to the IRQ pin at this capacitance and opens the gate. */
static esp_err_t lco_gate_open(int cap)
{
    esp_err_t err = i2c_dev_write_u8(s_dev, AS3935_REG_TUN,
                                     AS3935_TUN_DISP_LCO | (uint8_t)cap);
    if (err != ESP_OK) {
        return err;
    }
    pcnt_unit_clear_count(s_pcnt);
    s_sweep_us = esp_timer_get_time();
    s_sweep_cap = cap;
    return ESP_OK;
}

/* The IRQ handler must be off first: for the length of the sweep the pin
 * carries a 31 kHz clock, not events. */
static esp_err_t sweep_begin(void)
{
    gpio_intr_disable(AS3935_IRQ_GPIO);
    s_irq_pending = false;

    esp_err_t err = lco_counter_init();
    if (err != ESP_OK) {
        return err;
    }
    s_best_cap = 0;
    s_best_err = INT32_MAX;
    s_best_hz = 0;
    err = lco_gate_open(0);
    if (err != ESP_OK) {
        lco_counter_free();
    }
    return err;
}

/* One capacitance per call, ESP_ERR_NOT_FINISHED until the last: the gate is
 * 40 ms and the bus is free during it. */
static esp_err_t sweep_step(void)
{
    int64_t elapsed = esp_timer_get_time() - s_sweep_us;
    if (elapsed < AS3935_LCO_GATE_US) {
        return ESP_ERR_NOT_FINISHED;
    }

    int count = 0;
    esp_err_t err = pcnt_unit_get_count(s_pcnt, &count);
    if (err != ESP_OK) {
        lco_counter_free();
        return err;
    }

    int hz = (int)((int64_t)count * 1000000 / elapsed);
    int off = hz - AS3935_LCO_NOMINAL_HZ;
    if (off < 0) {
        off = -off;
    }
    if (off < s_best_err) {
        s_best_err = off;
        s_best_cap = s_sweep_cap;
        s_best_hz = (uint16_t)(hz > UINT16_MAX ? UINT16_MAX : hz);
    }
    ESP_LOGD(TAG, "TUN_CAP %2d: %d Hz", s_sweep_cap, hz);

    if (s_sweep_cap < AS3935_TUN_CAP_MAX) {
        err = lco_gate_open(s_sweep_cap + 1);
        if (err != ESP_OK) {
            lco_counter_free();
            return err;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    lco_counter_free();
    s_tun_cap = (uint8_t)s_best_cap;
    s_lco_hz = s_best_hz;

    if (s_best_err > AS3935_LCO_TOLERANCE_HZ) {
        ESP_LOGW(TAG, "antenna out of tune: best TUN_CAP %u gives %u Hz, "
                      "%d Hz off the %d Hz nominal",
                 s_tun_cap, s_lco_hz, s_best_err, AS3935_LCO_NOMINAL_HZ);
    } else {
        ESP_LOGI(TAG, "antenna tuned: TUN_CAP %u, LCO/16 %u Hz (%+d Hz)",
                 s_tun_cap, s_lco_hz, s_lco_hz - AS3935_LCO_NOMINAL_HZ);
    }
    /* Same write without DISP_LCO: the pin is an interrupt line again. */
    return i2c_dev_write_u8(s_dev, AS3935_REG_TUN, s_tun_cap);
}

/* ---------------- start ---------------- */

/* Every power-up needs this, and it needs the antenna tuned first — the RCOs
 * are trimmed against the LCO. */
static esp_err_t calibrate_rco(void)
{
    esp_err_t err = i2c_dev_write_u8(s_dev, AS3935_REG_CALIB_RCO, AS3935_DIRECT_CMD);
    if (err != ESP_OK) {
        return err;
    }
    /* The oscillators only settle while TRCO is routed to the IRQ pin — the
     * routing is what runs the calibration, not the display. */
    err = i2c_dev_write_u8(s_dev, AS3935_REG_TUN, AS3935_TUN_DISP_TRCO | s_tun_cap);
    if (err == ESP_OK) {
        esp_rom_delay_us(AS3935_SETTLE_US);
        err = i2c_dev_write_u8(s_dev, AS3935_REG_TUN, s_tun_cap);
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t trco, srco;
    err = i2c_dev_read(s_dev, AS3935_REG_CALIB_TRCO, &trco, 1);
    if (err == ESP_OK) {
        err = i2c_dev_read(s_dev, AS3935_REG_CALIB_SRCO, &srco, 1);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (!(trco & 0x80) || !(srco & 0x80)) {
        ESP_LOGW(TAG, "RCO calibration did not report done (TRCO 0x%02X, SRCO 0x%02X)",
                 trco, srco);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t fail(esp_err_t err)
{
    ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", AS3935_ADDR, s_step,
             esp_err_to_name(err));
    return err;
}

esp_err_t as3935_start(void)
{
    /* A sweep in progress owns the part until it ends; everything before it
     * has already run. */
    if (s_sweep_cap >= 0) {
        s_step = "antenna";
        esp_err_t err = sweep_step();
        if (err == ESP_ERR_NOT_FINISHED) {
            return err;
        }
        if (err != ESP_OK) {
            return fail(err);
        }
    } else {
        s_step = "probe";
        if (!i2c_dev_present(AS3935_ADDR)) {
            return ESP_ERR_NOT_FOUND;
        }
        if (!s_dev) {
            s_step = "attach";
            esp_err_t err = i2c_dev_attach(&s_dev, AS3935_ADDR);
            if (err != ESP_OK) {
                return fail(err);
            }
        }
        s_step = "irq_gpio";
        esp_err_t err = irq_gpio_configure();
        if (err != ESP_OK) {
            return fail(err);
        }
        /* Back to the factory register set first, so a restart after a failed
         * or half-written configuration starts from a known part. */
        s_step = "preset";
        err = i2c_dev_write_u8(s_dev, AS3935_REG_PRESET, AS3935_DIRECT_CMD);
        if (err != ESP_OK) {
            return fail(err);
        }
        esp_rom_delay_us(AS3935_SETTLE_US);

        s_step = "antenna";
        err = sweep_begin();
        if (err != ESP_OK) {
            return fail(err);
        }
        return ESP_ERR_NOT_FINISHED;
    }

    s_step = "calib_rco";
    esp_err_t err = calibrate_rco();
    if (err == ESP_OK) {
        s_step = "afe";
        err = i2c_dev_write_u8(s_dev, AS3935_REG_AFE, AS3935_AFE_GB_INDOOR << 1);
    }
    if (err == ESP_OK) {
        s_step = "noise_floor";
        s_nf_lev = AS3935_NF_LEV_DEFAULT;
        s_wdth = AS3935_WDTH_DEFAULT;
        err = write_nf(s_nf_lev, s_wdth);
    }
    if (err == ESP_OK) {
        s_step = "statistics";
        s_srej = AS3935_SREJ_DEFAULT;
        err = write_srej(s_srej);
    }
    if (err == ESP_OK) {
        /* MASK_DIST stays 0: the disturber rate is the input to the noise-floor
         * loop and an honest "it is electrically noisy here" signal. LCO_FDIV
         * stays 0 too — the sweep above measured against that divider. */
        s_step = "interrupt";
        err = i2c_dev_write_u8(s_dev, AS3935_REG_INT, 0x00);
    }
    if (err != ESP_OK) {
        return fail(err);
    }

    /* No ID register, and 0x03 is a bare address anything could answer on:
     * reading back a register we just wrote is the only identity check there
     * is. A device that stores what it is told still passes. */
    s_step = "verify";
    uint8_t nf;
    err = i2c_dev_read(s_dev, AS3935_REG_NF, &nf, 1);
    if (err != ESP_OK) {
        return fail(err);
    }
    if ((nf & 0x7F) != ((s_nf_lev << 4) | s_wdth)) {
        ESP_LOGW(TAG, "0x%02X ACKs but reg 0x01 reads back 0x%02X, not an AS3935",
                 AS3935_ADDR, nf);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Last, and only now: until here the pin was carrying calibration clocks. */
    s_step = "irq_attach";
    err = irq_attach();
    if (err != ESP_OK) {
        return fail(err);
    }

    s_step = "";
    s_last_noise_us = s_last_busy_us = s_hour_us = esp_timer_get_time();
    ESP_LOGI(TAG, "AS3935 at 0x%02X on IRQ GPIO%d, indoor gain %d, NF_LEV %u, "
                  "WDTH/SREJ %u/%u, TUN_CAP %u (%u Hz)",
             AS3935_ADDR, AS3935_IRQ_GPIO, AS3935_AFE_GB_INDOOR, s_nf_lev,
             s_wdth, s_srej, s_tun_cap, s_lco_hz);
    return ESP_OK;
}

/* ---------------- events ---------------- */

static void handle_lightning(int64_t event_us)
{
    uint8_t dist;
    uint8_t energy[3];
    if (i2c_dev_read(s_dev, AS3935_REG_DISTANCE, &dist, 1) != ESP_OK ||
        i2c_dev_read(s_dev, AS3935_REG_ENERGY, energy, sizeof(energy)) != ESP_OK) {
        return;
    }

    s_distance = dist & 0x3F;
    s_energy = ((uint32_t)(energy[2] & 0x1F) << 16) | ((uint32_t)energy[1] << 8) |
               energy[0];
    s_strikes++;
    s_hours[s_hour]++;
    s_last_strike_us = event_us;
    s_stats_dirty = true;

    if (s_distance == AS3935_DISTANCE_OUT_OF_RANGE) {
        ESP_LOGW(TAG, "lightning #%lu: out of range, energy %lu",
                 (unsigned long)s_strikes, (unsigned long)s_energy);
    } else if (s_distance == AS3935_DISTANCE_OVERHEAD) {
        ESP_LOGW(TAG, "lightning #%lu: overhead, energy %lu",
                 (unsigned long)s_strikes, (unsigned long)s_energy);
    } else {
        ESP_LOGW(TAG, "lightning #%lu: %u km, energy %lu",
                 (unsigned long)s_strikes, s_distance, (unsigned long)s_energy);
    }
}

/* "5" while it holds, "5 -> 6" on the window it moved. */
static const char *step_str(char *buf, size_t len, uint8_t from, uint8_t to)
{
    if (from == to) {
        snprintf(buf, len, "%u", to);
    } else {
        snprintf(buf, len, "%u -> %u", from, to);
    }
    return buf;
}

/* The noise floor is the one parameter worth adapting: it costs sensitivity
 * only while the interference lasts, and the part reports when it is too low. */
static void handle_noise(int64_t now)
{
    s_last_noise_us = now;
    if (s_nf_lev >= AS3935_NF_LEV_MAX) {
        return;
    }
    if (write_nf(s_nf_lev + 1, s_wdth) != ESP_OK) {
        return;
    }
    s_nf_lev++;
    ESP_LOGI(TAG, "noise floor too low, NF_LEV %u -> %u", s_nf_lev - 1, s_nf_lev);
}

/* Disturbers are man-made interference the part recognised and threw away, so
 * their rate is the honest measure of how hostile the spot is. WDTH first,
 * SREJ only after it has topped out: the watchdog throws away weak signals,
 * spike rejection reshapes what counts as a strike and costs more range. One
 * step per window either way; the caller logs what moved. */
static void adapt_rejection(uint16_t rate, int64_t now)
{
    if (rate >= AS3935_DISTURBERS_HIGH) {
        s_last_busy_us = now;
        if (s_wdth < AS3935_WDTH_MAX) {
            if (write_nf(s_nf_lev, s_wdth + 1) == ESP_OK) {
                s_wdth++;
            }
        } else if (s_srej < AS3935_SREJ_MAX) {
            if (write_srej(s_srej + 1) == ESP_OK) {
                s_srej++;
            }
        }
        return;
    }

    if (rate >= AS3935_DISTURBERS_LOW ||
        now - s_last_busy_us < (int64_t)AS3935_REJECT_DECAY_MS * 1000) {
        return;
    }
    if (s_srej > AS3935_SREJ_DEFAULT) {
        if (write_srej(s_srej - 1) != ESP_OK) {
            return;
        }
        s_srej--;
    } else if (s_wdth > AS3935_WDTH_DEFAULT) {
        if (write_nf(s_nf_lev, s_wdth - 1) != ESP_OK) {
            return;
        }
        s_wdth--;
    } else {
        return;
    }
    s_last_busy_us = now; /* one step per decay period, not per window */
}

static void decay_noise_floor(int64_t now)
{
    if (s_nf_lev <= AS3935_NF_LEV_DEFAULT ||
        now - s_last_noise_us < (int64_t)AS3935_NF_DECAY_MS * 1000) {
        return;
    }
    if (write_nf(s_nf_lev - 1, s_wdth) != ESP_OK) {
        return;
    }
    s_nf_lev--;
    s_last_noise_us = now;
    ESP_LOGI(TAG, "quiet, NF_LEV %u -> %u", s_nf_lev + 1, s_nf_lev);
}

/* Services the latched interrupt if one is due. Nothing to read is the normal
 * case — the state below is published either way. */
static esp_err_t service_irq(int64_t now)
{
    int64_t event_us;

    portENTER_CRITICAL(&s_irq_mux);
    /* The line stays high until the register is read, so an edge lost while
     * the handler was detached is still picked up here. */
    if (!s_irq_pending && gpio_get_level(AS3935_IRQ_GPIO)) {
        s_irq_pending = true;
        s_irq_us = now - AS3935_SETTLE_US;
    }
    /* The datasheet wants 2 ms between the event and the read; the edge is
     * timestamped in the handler, so an event caught too early waits for the
     * next call instead of being cleared unread. */
    bool due = s_irq_pending && now - s_irq_us >= AS3935_SETTLE_US;
    event_us = s_irq_us;
    if (due) {
        s_irq_pending = false;
    }
    portEXIT_CRITICAL(&s_irq_mux);

    if (!due) {
        return ESP_OK;
    }

    uint8_t reg;
    esp_err_t err = i2c_dev_read(s_dev, AS3935_REG_INT, &reg, 1);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t irq = reg & AS3935_INT_MASK;
    if (irq & AS3935_INT_LIGHTNING) {
        handle_lightning(event_us);
    }
    if (irq & AS3935_INT_DISTURBER) {
        s_disturbers++;
    }
    if (irq & AS3935_INT_NOISE) {
        handle_noise(now);
    }
    return ESP_OK;
}

/* One bucket per whole hour, however long the caller was away. */
static void roll_hours(int64_t now)
{
    int64_t hours = (now - s_hour_us) / ((int64_t)AS3935_HOUR_MS * 1000);
    if (hours <= 0) {
        return;
    }
    if (hours >= AS3935_STRIKE_HOURS) {
        memset(s_hours, 0, sizeof(s_hours));
        s_hour = 0;
    } else {
        for (int i = 0; i < (int)hours; i++) {
            s_hour = (s_hour + 1) % AS3935_STRIKE_HOURS;
            s_hours[s_hour] = 0;
        }
    }
    s_hour_us += hours * (int64_t)AS3935_HOUR_MS * 1000;
}

static uint16_t strikes_24h(void)
{
    int n = 0;
    for (int i = 0; i < AS3935_STRIKE_HOURS; i++) {
        n += s_hours[i];
    }
    return (uint16_t)(n > UINT16_MAX ? UINT16_MAX : n);
}

/* An hour of quiet is enough to make the part's own statistics stale; a day of
 * it drops the strike itself, so the reply stops carrying yesterday's storm. */
static void expire_last_strike(int64_t now)
{
    if (!s_last_strike_us) {
        return;
    }
    int64_t quiet_ms = (now - s_last_strike_us) / 1000;

    if (s_stats_dirty && quiet_ms >= AS3935_HOUR_MS) {
        s_stats_dirty = false;
        if (clear_statistics() == ESP_OK) {
            ESP_LOGI(TAG, "no lightning for an hour, statistics cleared");
        }
    }
    if (quiet_ms >= (int64_t)AS3935_HOUR_MS * AS3935_STRIKE_HOURS) {
        s_last_strike_us = 0;
        s_distance = AS3935_DISTANCE_OUT_OF_RANGE;
        s_energy = 0;
        ESP_LOGI(TAG, "no lightning for %d h, last strike forgotten",
                 AS3935_STRIKE_HOURS);
    }
}

esp_err_t as3935_read(as3935_data_t *out)
{
    if (!s_dev || s_sweep_cap >= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t now = esp_timer_get_time();
    esp_err_t err = service_irq(now);
    if (err != ESP_OK) {
        return err;
    }

    if (now - s_window_us >= (int64_t)AS3935_DISTURBER_WINDOW_MS * 1000) {
        s_disturbers_min = (uint16_t)s_disturbers;
        s_disturbers = 0;
        s_window_us = now;

        uint8_t wdth = s_wdth, srej = s_srej;
        adapt_rejection(s_disturbers_min, now);
        /* One line per window: the rate and what it did to the filters. Silent
         * only when nothing came in and nothing moved. */
        if (s_disturbers_min > 0 || s_wdth != wdth || s_srej != srej) {
            char w[12], r[12];
            ESP_LOGI(TAG, "%u disturber(s)/min, NF_LEV %u, WDTH %s, SREJ %s",
                     s_disturbers_min, s_nf_lev, step_str(w, sizeof(w), wdth, s_wdth),
                     step_str(r, sizeof(r), srej, s_srej));
        }
    }

    decay_noise_floor(now);
    roll_hours(now);
    expire_last_strike(now);

    out->last_strike_s = s_last_strike_us
                             ? (int32_t)((now - s_last_strike_us) / 1000000)
                             : -1;
    out->distance_km = s_distance;
    out->energy = s_energy;
    out->strikes_24h = strikes_24h();
    out->strikes = s_strikes;
    out->noise_floor = s_nf_lev;
    out->watchdog = s_wdth;
    out->spike_reject = s_srej;
    out->disturbers_min = s_disturbers_min;
    out->tun_cap = s_tun_cap;
    out->lco_hz = s_lco_hz;
    return ESP_OK;
}
