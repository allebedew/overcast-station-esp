#include "encoder.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "button_gpio.h"
#include "iot_button.h"

#define PIN_A  23
#define PIN_B  22
#define PIN_SW 21

#define LONG_PRESS_MS 800

/* Quadrature counts per detent: an EC11 rests with both contacts closed and
 * passes through all four states between one detent and the next. */
#define DETENT 4

/* Rotation is sampled far faster than a frame is drawn. A frame period would
 * be too coarse to split by direction — a genuine reversal inside the window
 * would net out and vanish; at 10 ms no hand can turn back that fast. */
#define POLL_MS 10

/* The hardware counter wraps at these and accumulates into a 32-bit total, so
 * nothing is lost between two polls however long the gap. */
#define PCNT_LIM 1000

/* A formality against pickup on an unshielded wire, not debouncing: the filter
 * is capped near 12.8 us on this chip (10 bits of APB) while contact bounce
 * lasts milliseconds. Bounce is rejected by the quadrature itself — a
 * chattering contact counts +1/-1 around the detent and cancels out, which is
 * why both edges of both channels are decoded rather than one. */
#define GLITCH_NS 1000

static const char *TAG = "encoder";

static pcnt_unit_handle_t s_unit;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static encoder_input_t s_acc;

/* The poll timer and the button callbacks both run in the esp_timer task, so
 * these two are shared between them without a lock; only s_acc crosses into
 * the caller's task. */
static bool s_held;
static bool s_turned_while_held;

static void add8(int8_t *dst, int n)
{
    int v = *dst + n;
    *dst = v > INT8_MAX ? INT8_MAX : (int8_t)v;
}

static void poll(void *arg)
{
    static int last;      /* the accumulated total at the previous poll */
    static int remainder; /* counts not yet worth a detent */

    int total = 0;
    pcnt_unit_get_count(s_unit, &total);
    remainder += total - last;
    last = total;

    int detents = remainder / DETENT; /* truncates toward zero, both signs */
    if (detents == 0) {
        return;
    }
    remainder -= detents * DETENT;

    if (s_held) {
        s_turned_while_held = true;
    }

    portENTER_CRITICAL(&s_mux);
    if (detents > 0) {
        add8(s_held ? &s_acc.cw_held : &s_acc.cw, detents);
    } else {
        add8(s_held ? &s_acc.ccw_held : &s_acc.ccw, -detents);
    }
    portEXIT_CRITICAL(&s_mux);
}

static void on_press_down(void *arg, void *usr_data)
{
    s_held = true;
    s_turned_while_held = false;
}

static void on_press_up(void *arg, void *usr_data)
{
    s_held = false;
}

/* Turning with the button down is a gesture of its own; the release that ends
 * it is not also a press. */
static void on_click(void *arg, void *usr_data)
{
    if (s_turned_while_held) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_acc.click++;
    portEXIT_CRITICAL(&s_mux);
}

static void on_double_click(void *arg, void *usr_data)
{
    if (s_turned_while_held) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_acc.double_click++;
    portEXIT_CRITICAL(&s_mux);
}

static void on_long_press(void *arg, void *usr_data)
{
    portENTER_CRITICAL(&s_mux);
    s_acc.long_press++;
    portEXIT_CRITICAL(&s_mux);
}

void encoder_take(encoder_input_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_acc;
    memset(&s_acc, 0, sizeof(s_acc));
    portEXIT_CRITICAL(&s_mux);

    out->held = s_held;
}

static void pcnt_start(void)
{
    pcnt_unit_config_t unit_cfg = {
        .low_limit = -PCNT_LIM,
        .high_limit = PCNT_LIM,
        .flags.accum_count = true,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_unit));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_unit, -PCNT_LIM));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_unit, PCNT_LIM));

    pcnt_glitch_filter_config_t filter = { .max_glitch_ns = GLITCH_NS };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_unit, &filter));

    /* Each channel counts one line's edges with the other line deciding the
     * sign, so all four transitions of the cycle are decoded. Swap the two
     * configurations to reverse which way is clockwise — the wiring stays. */
    pcnt_channel_handle_t chan_a = NULL, chan_b = NULL;
    pcnt_chan_config_t cfg_a = { .edge_gpio_num = PIN_A, .level_gpio_num = PIN_B };
    pcnt_chan_config_t cfg_b = { .edge_gpio_num = PIN_B, .level_gpio_num = PIN_A };
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &cfg_a, &chan_a));
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &cfg_b, &chan_b));

    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_a,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_b,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_b,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_unit));
}

/* The button driver polls every CONFIG_BUTTON_PERIOD_TIME_MS from a timer it
 * already runs for BOOT, which is both the debounce and the reason there is no
 * interrupt here. Its internal pull-up is what the switch line has: the
 * module's own 47k pull-ups are unpopulated and it has no supply. */
static void button_start(void)
{
    const button_config_t btn_cfg = {};
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = PIN_SW,
        .active_level = 0,
    };

    button_handle_t btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL, on_press_down, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_PRESS_UP, NULL, on_press_up, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, on_click, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, on_double_click, NULL));

    button_event_args_t long_press = { .long_press.press_time = LONG_PRESS_MS };
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, &long_press,
                                           on_long_press, NULL));
}

void encoder_init(void)
{
    pcnt_start();
    button_start();

    const esp_timer_create_args_t timer = {
        .callback = poll,
        .name = "encoder",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_handle_t h = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&timer, &h));
    ESP_ERROR_CHECK(esp_timer_start_periodic(h, POLL_MS * 1000));

    ESP_LOGI(TAG, "Encoder ready, A %d B %d SW %d, %d counts per detent",
             PIN_A, PIN_B, PIN_SW, DETENT);
}
