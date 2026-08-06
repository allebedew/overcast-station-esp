#include "as3935.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "i2c_dev.h"

/* Set by the module's ADD0/ADD1 jumpers; 0x01 and 0x02 are the alternatives if
 * anything ever collides. */
#define AS3935_ADDR 0x03

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

#define AS3935_INT_MASK      0x0F
#define AS3935_INT_NOISE     0x01
#define AS3935_INT_DISTURBER 0x04
#define AS3935_INT_LIGHTNING 0x08

/* Indoor gain. Outdoor is 14, and getting this wrong is fatal either way:
 * indoor gain outdoors saturates on everything, outdoor gain indoors hears
 * nothing. It becomes a setting once the sensor's final home is known. */
#define AS3935_AFE_GB_INDOOR 18

/* Antenna is untuned: the tank runs at whatever the module left the factory
 * with. Tuning needs the resonance frequency, which only comes out on the IRQ
 * pin, so it stays 0 until that pin is temporarily wired. */
#define AS3935_TUN_CAP 0

/* Watchdog threshold and spike rejection at their defaults. They trade
 * sensitivity for immunity silently — nothing here is allowed to move them,
 * and raising them is a last resort after NF_LEV has topped out. */
#define AS3935_WDTH 2
#define AS3935_SREJ 2

/* Noise floor: where it starts, and how far the adaptation may push it. */
#define AS3935_NF_LEV_DEFAULT 2
#define AS3935_NF_LEV_MAX     7

/* Reserved bit 7 reads 1; CL_STAT 1, MIN_NUM_LIGH 0 (= one strike, no
 * accumulation — accumulation hides an isolated distant storm). */
#define AS3935_STAT_BASE (0xC0 | AS3935_SREJ)
#define AS3935_STAT_CL_STAT 0x40

/* Datasheet: 2 ms for the direct commands and for the TRCO settling pulse.
 * Busy-waited — the FreeRTOS tick is 10 ms, so a short vTaskDelay is a no-op. */
#define AS3935_SETTLE_US 2500

/* Quiet before the noise floor may step back down. Long, because the point of
 * lowering it is sensitivity, not a fast reaction. */
#define AS3935_NF_DECAY_MS (10 * 60 * 1000)

/* How long a strike keeps the storm flag up, and when the part's statistics
 * are cleared. The distance register holds its last estimate forever, so
 * without this the state would show a storm that has long since passed. */
#define AS3935_STORM_HOLD_MS (60 * 60 * 1000)

#define AS3935_DISTURBER_WINDOW_MS 60000

static const char *TAG = "as3935";

static i2c_master_dev_handle_t s_dev;

static uint8_t s_nf_lev = AS3935_NF_LEV_DEFAULT;
static uint32_t s_strikes;
static uint8_t s_distance = AS3935_DISTANCE_OUT_OF_RANGE;
static uint32_t s_energy;

static int64_t s_last_strike_us;   /* 0 = none since start */
static int64_t s_last_noise_us;
static bool s_storm;

/* Disturbers are counted over a window and published as the rate of the last
 * completed one, so the number does not sag to zero between events. */
static int s_disturbers;
static uint16_t s_disturbers_min;
static int64_t s_window_us;

/* Where start() gave up, so the warning below can name the step. */
static const char *s_step = "";

static esp_err_t write_nf_lev(uint8_t nf_lev)
{
    return i2c_dev_write_u8(s_dev, AS3935_REG_NF, (nf_lev << 4) | AS3935_WDTH);
}

/* CL_STAT is a level, not a command: it clears the accumulated statistics on a
 * high-low-high transition. */
static esp_err_t clear_statistics(void)
{
    esp_err_t err = i2c_dev_write_u8(s_dev, AS3935_REG_STAT,
                                     AS3935_STAT_BASE & ~AS3935_STAT_CL_STAT);
    if (err == ESP_OK) {
        err = i2c_dev_write_u8(s_dev, AS3935_REG_STAT, AS3935_STAT_BASE);
    }
    return err;
}

/* Every power-up needs this; the LCO/antenna calibration is a different thing
 * entirely and cannot be done over I2C at all. */
static esp_err_t calibrate_rco(void)
{
    esp_err_t err = i2c_dev_write_u8(s_dev, AS3935_REG_CALIB_RCO, AS3935_DIRECT_CMD);
    if (err != ESP_OK) {
        return err;
    }
    /* The oscillators only settle while TRCO is routed to the (unwired) IRQ
     * pin — the routing is what runs the calibration, not the display. */
    err = i2c_dev_write_u8(s_dev, AS3935_REG_TUN, 0x20 | AS3935_TUN_CAP);
    if (err == ESP_OK) {
        esp_rom_delay_us(AS3935_SETTLE_US);
        err = i2c_dev_write_u8(s_dev, AS3935_REG_TUN, AS3935_TUN_CAP);
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

esp_err_t as3935_start(void)
{
    s_step = "probe";
    if (!i2c_dev_present(AS3935_ADDR)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_dev) {
        s_step = "attach";
        esp_err_t err = i2c_dev_attach(&s_dev, AS3935_ADDR);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", AS3935_ADDR,
                     s_step, esp_err_to_name(err));
            return err;
        }
    }

    s_step = "preset";
    esp_err_t err = i2c_dev_write_u8(s_dev, AS3935_REG_PRESET, AS3935_DIRECT_CMD);
    if (err == ESP_OK) {
        esp_rom_delay_us(AS3935_SETTLE_US);
        s_step = "calib_rco";
        err = calibrate_rco();
    }
    if (err == ESP_OK) {
        s_step = "afe";
        err = i2c_dev_write_u8(s_dev, AS3935_REG_AFE, AS3935_AFE_GB_INDOOR << 1);
    }
    if (err == ESP_OK) {
        s_step = "noise_floor";
        s_nf_lev = AS3935_NF_LEV_DEFAULT;
        err = write_nf_lev(s_nf_lev);
    }
    if (err == ESP_OK) {
        s_step = "statistics";
        err = i2c_dev_write_u8(s_dev, AS3935_REG_STAT, AS3935_STAT_BASE);
    }
    if (err == ESP_OK) {
        /* MASK_DIST stays 0: the disturber rate is the input to the noise-floor
         * loop and an honest "it is electrically noisy here" signal, and
         * masking saves nothing when there is no interrupt line to spare. */
        s_step = "interrupt";
        err = i2c_dev_write_u8(s_dev, AS3935_REG_INT, 0x00);
    }
    if (err == ESP_OK) {
        s_step = "tuning";
        err = i2c_dev_write_u8(s_dev, AS3935_REG_TUN, AS3935_TUN_CAP);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", AS3935_ADDR,
                 s_step, esp_err_to_name(err));
        return err;
    }

    /* No ID register, and 0x03 is a bare address anything could answer on:
     * reading back a register we just wrote is the only identity check there
     * is. A device that stores what it is told still passes. */
    s_step = "verify";
    uint8_t nf;
    err = i2c_dev_read(s_dev, AS3935_REG_NF, &nf, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", AS3935_ADDR,
                 s_step, esp_err_to_name(err));
        return err;
    }
    if ((nf & 0x7F) != ((s_nf_lev << 4) | AS3935_WDTH)) {
        ESP_LOGW(TAG, "0x%02X ACKs but reg 0x01 reads back 0x%02X, not an AS3935",
                 AS3935_ADDR, nf);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_step = "";
    s_last_noise_us = esp_timer_get_time();
    ESP_LOGI(TAG, "AS3935 at 0x%02X, indoor gain %d, NF_LEV %u, WDTH/SREJ %d/%d, "
                  "TUN_CAP %d (untuned), polled without IRQ",
             AS3935_ADDR, AS3935_AFE_GB_INDOOR, s_nf_lev, AS3935_WDTH, AS3935_SREJ,
             AS3935_TUN_CAP);
    return ESP_OK;
}

static void handle_lightning(void)
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
    s_last_strike_us = esp_timer_get_time();
    s_storm = true;

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

/* The noise floor is the one parameter worth adapting: it costs sensitivity
 * only while the interference lasts, and the part reports when it is too low. */
static void handle_noise(int64_t now)
{
    s_last_noise_us = now;
    if (s_nf_lev >= AS3935_NF_LEV_MAX) {
        return;
    }
    if (write_nf_lev(s_nf_lev + 1) != ESP_OK) {
        return;
    }
    s_nf_lev++;
    ESP_LOGI(TAG, "noise floor too low, NF_LEV %u -> %u", s_nf_lev - 1, s_nf_lev);
}

static void decay_noise_floor(int64_t now)
{
    if (s_nf_lev <= AS3935_NF_LEV_DEFAULT ||
        now - s_last_noise_us < (int64_t)AS3935_NF_DECAY_MS * 1000) {
        return;
    }
    if (write_nf_lev(s_nf_lev - 1) != ESP_OK) {
        return;
    }
    s_nf_lev--;
    s_last_noise_us = now;
    ESP_LOGI(TAG, "quiet, NF_LEV %u -> %u", s_nf_lev + 1, s_nf_lev);
}

esp_err_t as3935_read(as3935_data_t *out)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Reading this register is what clears the latched interrupt. The
     * datasheet asks for 2 ms between the event and the read; polling cannot
     * honour that, so an event caught inside its own settling window may read
     * as none. At a 50 ms period that is a few percent of them. */
    uint8_t reg;
    esp_err_t err = i2c_dev_read(s_dev, AS3935_REG_INT, &reg, 1);
    if (err != ESP_OK) {
        return err;
    }

    int64_t now = esp_timer_get_time();
    uint8_t irq = reg & AS3935_INT_MASK;

    if (irq & AS3935_INT_LIGHTNING) {
        handle_lightning();
    }
    if (irq & AS3935_INT_DISTURBER) {
        s_disturbers++;
    }
    if (irq & AS3935_INT_NOISE) {
        handle_noise(now);
    }

    if (now - s_window_us >= (int64_t)AS3935_DISTURBER_WINDOW_MS * 1000) {
        if (s_disturbers > 0) {
            ESP_LOGI(TAG, "%d disturber(s) in the last minute, NF_LEV %u",
                     s_disturbers, s_nf_lev);
        }
        s_disturbers_min = (uint16_t)s_disturbers;
        s_disturbers = 0;
        s_window_us = now;
    }

    decay_noise_floor(now);

    /* Nothing in the part expires: the distance register keeps its last
     * estimate and the accumulated energy keeps feeding it, so the end of a
     * storm has to be declared here. */
    if (s_storm && now - s_last_strike_us >= (int64_t)AS3935_STORM_HOLD_MS * 1000) {
        s_storm = false;
        s_distance = AS3935_DISTANCE_OUT_OF_RANGE;
        s_energy = 0;
        if (clear_statistics() == ESP_OK) {
            ESP_LOGI(TAG, "no lightning for %d min, statistics cleared",
                     AS3935_STORM_HOLD_MS / 60000);
        }
    }

    out->storm = s_storm;
    out->distance_km = s_distance;
    out->overhead = s_storm && s_distance == AS3935_DISTANCE_OVERHEAD;
    out->out_of_range = s_distance == AS3935_DISTANCE_OUT_OF_RANGE;
    out->energy = s_energy;
    out->strikes = s_strikes;
    out->last_strike_s = s_last_strike_us
                             ? (int32_t)((now - s_last_strike_us) / 1000000)
                             : -1;
    out->noise_floor = s_nf_lev;
    out->disturbers_min = s_disturbers_min;
    return ESP_OK;
}
