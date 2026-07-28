#include "button.h"

#include "esp_log.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "screen_16x2.h"
#include "wifi.h"

#define BUTTON_GPIO 9 /* кнопка BOOT */
#define LONG_PRESS_MS 1500

static const char *TAG = "button";

/* Short press browses the display; toggling the access point sits on a long
 * press, where an accidental tap cannot trigger it. */
static void on_button_click(void *arg, void *usr_data)
{
    ESP_LOGI(TAG, "BOOT button click: next screen page");
    screen_16x2_next_page();
}

static void on_button_long_press(void *arg, void *usr_data)
{
    wifi_info_t info;
    wifi_get_info(&info);
    ESP_LOGI(TAG, "BOOT button long press: AP %s",
             info.ap_active ? "off" : "on");
    wifi_ap_enable(!info.ap_active);
}

void button_init(void)
{
    const button_config_t btn_cfg = {};
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = BUTTON_GPIO,
        .active_level = 0,
    };

    button_handle_t btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, on_button_click, NULL));

    /* Fires while the button is still down, so the AP toggles as soon as the
     * hold is long enough — no need to guess when to let go. */
    button_event_args_t long_press = { .long_press.press_time = LONG_PRESS_MS };
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, &long_press,
                                           on_button_long_press, NULL));

    ESP_LOGI(TAG, "Button ready, GPIO %d (click: screen page, hold %d ms: AP)",
             BUTTON_GPIO, LONG_PRESS_MS);
}
