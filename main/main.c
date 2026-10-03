/*
 * main.c - PHASE 8: TEMP + diagnostics + software clock.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_types.h"
#include "app_queues.h"
#include "i2c_bus.h"
#include "task_button.h"
#include "task_clock.h"
#include "task_temp.h"
#include "task_mode.h"
#include "task_display.h"
#include "task_alarm.h"
#include "storage.h"
#include "diag.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Multi-function clock - Phase 8 (TEMP + Diagnostics + Software clock)");

    ESP_ERROR_CHECK(i2c_bus_init(I2C_SDA_GPIO, I2C_SCL_GPIO));
    (void)i2c_bus_scan();
    ESP_ERROR_CHECK(app_queues_create());
    ESP_ERROR_CHECK(storage_init());

    ESP_ERROR_CHECK(task_display_start());
    ESP_ERROR_CHECK(task_mode_start());
    ESP_ERROR_CHECK(task_clock_start());
    ESP_ERROR_CHECK(task_temp_start());
    ESP_ERROR_CHECK(task_alarm_start());
    ESP_ERROR_CHECK(task_button_start());
    ESP_ERROR_CHECK(diag_start());

    ESP_LOGI(TAG, "All tasks started. LEFT/RIGHT = change mode, hold CENTER = edit (clock/alarm/countdown).");
}
