/*
 * task_temp.c - TempTask
 *   Mục đích : đọc DHT mỗi 2 s, phát EVT_ENV, nếu DHT lỗi thì dùng nhiệt độ RTC làm dự phòng.
 *   Độ ưu tiên: PRIO_TEMP (1)
 *   Chu kỳ: TEMP_PERIOD_MS (2000 ms), dùng vTaskDelayUntil
 */
#include "task_temp.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_types.h"
#include "app_queues.h"
#include "i2c_bus.h"
#include "dht.h"
#include "ds3231.h"

static const char *TAG = "TEMP";

#define TEMP_FIRST_DELAY_MS 1500U
#define DHT_FAILS_BEFORE_RTC 2U

#ifdef DHT_TYPE_DHT22
#define DHT_MODEL_CHOSEN DHT_MODEL_DHT22
#else
#define DHT_MODEL_CHOSEN DHT_MODEL_DHT11
#endif

static void send_env(float temp_c, float humidity_pct, bool humidity_valid, bool from_rtc)
{
    app_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = EVT_ENV;
    evt.data.env.temp_c = temp_c;
    evt.data.env.humidity_pct = humidity_pct;
    evt.data.env.humidity_valid = humidity_valid;
    evt.data.env.from_rtc = from_rtc;
    (void)app_event_send(&evt, "TempTask");
}

static void temp_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(TEMP_FIRST_DELAY_MS));
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t fail_streak = 0;

    while (1) {
        float t = 0.0f;
        float h = 0.0f;
        esp_err_t err = ESP_ERR_TIMEOUT;
        if (i2c_bus_lock(I2C_MUTEX_TIMEOUT_MS)) {
            err = dht_read(&t, &h);
            i2c_bus_unlock();
        }

        if (err == ESP_OK) {
            fail_streak = 0;
            send_env(t, h, true, false);
        } else {
            fail_streak++;
            if (fail_streak == 1U || (fail_streak % 10U) == 0U) {
                ESP_LOGW(TAG, "DHT read failed: %s (streak %lu)", esp_err_to_name(err),
                         (unsigned long)fail_streak);
            }
            if (fail_streak >= DHT_FAILS_BEFORE_RTC) {
                float rt = 0.0f;
                esp_err_t rerr = ESP_ERR_TIMEOUT;
                if (i2c_bus_lock(I2C_MUTEX_TIMEOUT_MS)) {
                    rerr = ds3231_get_temperature(&rt);
                    i2c_bus_unlock();
                }
                if (rerr == ESP_OK) {
                    send_env(rt, 0.0f, false, true);
                } else {
                    ESP_LOGW(TAG, "DS3231 temperature failed: %s", esp_err_to_name(rerr));
                }
            }
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TEMP_PERIOD_MS));
    }
}

esp_err_t task_temp_start(void)
{
    esp_err_t err = dht_init(DHT_GPIO, DHT_MODEL_CHOSEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dht_init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(temp_task, "TempTask", STACK_TEMP, NULL, PRIO_TEMP, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create TempTask");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TempTask started (period %d ms)", TEMP_PERIOD_MS);
    return ESP_OK;
}
