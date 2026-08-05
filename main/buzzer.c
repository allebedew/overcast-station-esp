#include "buzzer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "settings.h"

#define BUZZER_GPIO 19

#define LEDC_MODE    LEDC_LOW_SPEED_MODE /* the only mode this chip has */
#define LEDC_TIMER   LEDC_TIMER_0
#define LEDC_CHANNEL LEDC_CHANNEL_0

/* Below ~2% of full swing the pulses get short enough for the tone to go
 * uneven — quieter than that wants a resistor in series with the piezo. */
#define DEFAULT_VOLUME_PCT 10
#define DUTY_RES           LEDC_TIMER_10_BIT
#define DUTY_MAX           (1 << 10)
#define DUTY_OFF           0

#define SETTING_VOLUME "buzz_vol"

/* Idle frequency: the timer needs one to be configured, nothing is audible at
 * DUTY_OFF. */
#define IDLE_HZ 4000

static const char *TAG = "buzzer";

/* Note durations are multiples of the 10 ms FreeRTOS tick — anything finer is
 * rounded up by the delay below. */
typedef struct {
    uint16_t hz; /* 0 = rest */
    uint16_t ms;
} note_t;

typedef struct {
    const note_t *notes;
    uint8_t count;
} tune_t;

#define REST(ms) { 0, (ms) }
#define TUNE(a)  { a, sizeof(a) / sizeof((a)[0]) }

/* KPT-1410 resonates at 4 kHz +-0.5 and drops off steeply outside that band,
 * so every tune stays inside it and differs by rhythm instead of pitch. The
 * three clicks are the exception: they exist to be compared, and the outer two
 * are deliberately off resonance, which makes them quieter as well as
 * different. */
static const note_t CLICK_LO_NOTES[] = { { 3200, 20 } };
static const note_t CLICK_NOTES[]    = { { 4000, 20 } };
static const note_t CLICK_HI_NOTES[] = { { 4800, 20 } };

/* Long enough to judge the loudness, not so long it becomes a nuisance. */
static const note_t TONE_2K5_NOTES[] = { { 2500, 20 } };
static const note_t TONE_1K5_NOTES[] = { { 1500, 20 } };

static const note_t OK_NOTES[]    = { { 3600, 50 }, { 4400, 70 } };
static const note_t WARN_NOTES[]  = { { 4000, 120 }, REST(90), { 4000, 120 } };
static const note_t ALARM_NOTES[] = { { 4400, 150 }, { 3600, 150 },
                                      { 4400, 150 }, { 3600, 150 },
                                      { 4400, 150 }, { 3600, 150 } };
static const note_t ERROR_NOTES[] = { { 4000, 50 }, REST(40), { 4000, 50 },
                                      REST(40), { 3600, 300 } };

/* Built from the same 120 ms beep as WARN, so the air alerts sound like one
 * family; the pitch pair carries the direction. One motif per zone edge
 * crossed, and the gap between motifs is wide enough to count them by ear.
 * The last motif of the top zone is held longer — that is the one worth
 * interrupting somebody for. */
#define CO2_UP   { 3600, 100 }, { 4300, 100 }
#define CO2_DOWN { 4300, 100 }, { 3600, 100 }
#define CO2_GAP  REST(100)

static const note_t CO2_UP1_NOTES[] = { CO2_UP };
static const note_t CO2_UP2_NOTES[] = { CO2_UP, CO2_GAP, CO2_UP };
static const note_t CO2_UP3_NOTES[] = { CO2_UP, CO2_GAP, CO2_UP, CO2_GAP,
                                        { 3600, 100 }, { 4300, 150 } };
static const note_t CO2_DOWN1_NOTES[] = { CO2_DOWN };
static const note_t CO2_DOWN2_NOTES[] = { CO2_DOWN, CO2_GAP, CO2_DOWN };
static const note_t CO2_DOWN3_NOTES[] = { CO2_DOWN, CO2_GAP, CO2_DOWN,
                                          CO2_GAP, CO2_DOWN };

static const note_t STORM_NOTES[]  = { { 4800, 20 }, REST(40), { 4800, 20 },
                                       REST(40), { 4800, 20 }, REST(70),
                                       { 3200, 250 } };
/* The two notes are split by a rest rather than run together: back to back the
 * pair reads as one warbling beep and the direction goes with it. */
static const note_t ARRIVE_NOTES[] = { { 3800, 40 }, REST(50), { 4600, 80 } };
static const note_t LEAVE_NOTES[]  = { { 4600, 40 }, REST(50), { 3800, 80 } };

static const note_t BOOT_NOTES[] = { { 3500, 60 }, { 4000, 60 }, { 4500, 90 } };

static const tune_t TUNES[BUZZER_TUNE_COUNT] = {
    [BUZZER_CLICK_LO]  = TUNE(CLICK_LO_NOTES),
    [BUZZER_CLICK]     = TUNE(CLICK_NOTES),
    [BUZZER_CLICK_HI]  = TUNE(CLICK_HI_NOTES),
    [BUZZER_TONE_2K5]  = TUNE(TONE_2K5_NOTES),
    [BUZZER_TONE_1K5]  = TUNE(TONE_1K5_NOTES),
    [BUZZER_OK]        = TUNE(OK_NOTES),
    [BUZZER_WARN]      = TUNE(WARN_NOTES),
    [BUZZER_ALARM]     = TUNE(ALARM_NOTES),
    [BUZZER_ERROR]     = TUNE(ERROR_NOTES),
    [BUZZER_CO2_UP1]   = TUNE(CO2_UP1_NOTES),
    [BUZZER_CO2_UP2]   = TUNE(CO2_UP2_NOTES),
    [BUZZER_CO2_UP3]   = TUNE(CO2_UP3_NOTES),
    [BUZZER_CO2_DOWN1] = TUNE(CO2_DOWN1_NOTES),
    [BUZZER_CO2_DOWN2] = TUNE(CO2_DOWN2_NOTES),
    [BUZZER_CO2_DOWN3] = TUNE(CO2_DOWN3_NOTES),
    [BUZZER_STORM]     = TUNE(STORM_NOTES),
    [BUZZER_ARRIVE]    = TUNE(ARRIVE_NOTES),
    [BUZZER_LEAVE]     = TUNE(LEAVE_NOTES),
    [BUZZER_BOOT]      = TUNE(BOOT_NOTES),
};

static const char *const TUNE_NAMES[BUZZER_TUNE_COUNT] = {
    [BUZZER_CLICK_LO]  = "Click lo",
    [BUZZER_CLICK]     = "Click",
    [BUZZER_CLICK_HI]  = "Click hi",
    [BUZZER_TONE_2K5]  = "Tone 2.5k",
    [BUZZER_TONE_1K5]  = "Tone 1.5k",
    [BUZZER_OK]        = "OK",
    [BUZZER_WARN]      = "Warn",
    [BUZZER_ALARM]     = "Alarm",
    [BUZZER_ERROR]     = "Error",
    [BUZZER_CO2_UP1]   = "CO2 up 1",
    [BUZZER_CO2_UP2]   = "CO2 up 2",
    [BUZZER_CO2_UP3]   = "CO2 up 3",
    [BUZZER_CO2_DOWN1] = "CO2 dn 1",
    [BUZZER_CO2_DOWN2] = "CO2 dn 2",
    [BUZZER_CO2_DOWN3] = "CO2 dn 3",
    [BUZZER_STORM]     = "Storm",
    [BUZZER_ARRIVE]    = "Arrive",
    [BUZZER_LEAVE]     = "Leave",
    [BUZZER_BOOT]      = "Boot",
};

#define REQ_STOP (-1)

static QueueHandle_t s_queue; /* length 1, overwritten: the newest request wins */
static volatile uint8_t s_volume_pct = DEFAULT_VOLUME_PCT;

static void output(const note_t *note)
{
    if (note && note->hz) {
        ledc_set_freq(LEDC_MODE, LEDC_TIMER, note->hz);
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, DUTY_MAX * s_volume_pct / 100);
    } else {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, DUTY_OFF);
    }
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

/* Waits out the current note, but on the queue rather than in a delay, so a
 * request arriving mid-note replaces the tune instead of being heard after it.
 */
static void buzzer_task(void *arg)
{
    const note_t *note = NULL, *end = NULL;

    for (;;) {
        int8_t req;
        TickType_t wait = note ? pdMS_TO_TICKS(note->ms) : portMAX_DELAY;

        if (xQueueReceive(s_queue, &req, wait) == pdTRUE) {
            note = req == REQ_STOP ? NULL : TUNES[req].notes;
            end = note ? note + TUNES[req].count : NULL;
        } else if (++note == end) {
            note = NULL;
        }
        output(note);
    }
}

void buzzer_play(buzzer_tune_t tune)
{
    if (!s_queue || (unsigned)tune >= BUZZER_TUNE_COUNT) {
        return;
    }
    int8_t req = (int8_t)tune;
    xQueueOverwrite(s_queue, &req);
}

void buzzer_stop(void)
{
    if (!s_queue) {
        return;
    }
    int8_t req = REQ_STOP;
    xQueueOverwrite(s_queue, &req);
}

const char *buzzer_tune_name(buzzer_tune_t tune)
{
    return (unsigned)tune < BUZZER_TUNE_COUNT ? TUNE_NAMES[tune] : "";
}

void buzzer_set_volume(uint8_t pct)
{
    if (pct < BUZZER_VOL_MIN) {
        pct = BUZZER_VOL_MIN;
    } else if (pct > BUZZER_VOL_MAX) {
        pct = BUZZER_VOL_MAX;
    }
    s_volume_pct = pct;
}

uint8_t buzzer_get_volume(void)
{
    return s_volume_pct;
}

void buzzer_save_volume(void)
{
    settings_set_u8(SETTING_VOLUME, s_volume_pct);
}

void buzzer_init(void)
{
    /* The piezo floats until LEDC claims the pin, and driving it hard to ground
     * discharges its 10 nF in well under a microsecond -- an audible click. The
     * internal pull-down bleeds it off over ~0.5 ms (45 kOhm x 10 nF) instead,
     * which puts the step's energy far below the element's 4 kHz band; three
     * time constants later the pin is at ground and LEDC changes nothing. */
    gpio_config_t idle = {
        .pin_bit_mask = BIT64(BUZZER_GPIO),
        .mode = GPIO_MODE_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&idle));
    vTaskDelay(pdMS_TO_TICKS(10));

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = DUTY_RES,
        .freq_hz = IDLE_HZ,
        /* XTAL, not APB: on the APB clock the pitch would shift with the CPU
         * frequency under DFS. */
        .clk_cfg = LEDC_USE_XTAL_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BUZZER_GPIO,
        .duty = DUTY_OFF,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    buzzer_set_volume(settings_get_u8(SETTING_VOLUME, DEFAULT_VOLUME_PCT));

    s_queue = xQueueCreate(1, sizeof(int8_t));
    ESP_ERROR_CHECK(s_queue ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskCreate(buzzer_task, "buzzer", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "buzzer on GPIO%d, volume %u%%", BUZZER_GPIO, s_volume_pct);
}
