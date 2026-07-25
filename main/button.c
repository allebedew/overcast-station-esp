#include "button.h"

#include "esp_log.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "wifi.h"

#define BUTTON_GPIO 9 /* кнопка BOOT */

static const char *TAG = "button";

static void on_button_click(void *arg, void *usr_data)
{
    ESP_LOGI(TAG, "BOOT button pressed");

    wifi_info_t info;
    wifi_get_info(&info);
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

    ESP_LOGI(TAG, "Button ready, GPIO %d", BUTTON_GPIO);
}
