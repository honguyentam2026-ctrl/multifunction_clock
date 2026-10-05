#include "app_queues.h"

#include "esp_log.h"
#include "app_config.h"
#include "app_types.h"

static const char *TAG = "QUEUE";

QueueHandle_t eventQueue = NULL;
QueueHandle_t displayQueue = NULL;
QueueHandle_t alarmQueue = NULL;
QueueHandle_t rtcCmdQueue = NULL;
QueueSetHandle_t clockSet = NULL;
SemaphoreHandle_t clockTickSem = NULL;

static uint32_t s_drop_count = 0;
static portMUX_TYPE s_drop_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t app_queues_create(void)
{
    eventQueue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(app_event_t));
    displayQueue = xQueueCreate(DISPLAY_QUEUE_LEN, sizeof(display_msg_t));
    alarmQueue = xQueueCreate(ALARM_QUEUE_LEN, sizeof(alarm_cmd_t));
    rtcCmdQueue = xQueueCreate(RTC_CMD_QUEUE_LEN, sizeof(rtc_cmd_t));

    if (eventQueue == NULL || displayQueue == NULL || alarmQueue == NULL || rtcCmdQueue == NULL) {
        ESP_LOGE(TAG, "queue creation failed (out of memory)");
        return ESP_ERR_NO_MEM;
    }

    clockTickSem = xSemaphoreCreateBinary();
    clockSet = xQueueCreateSet(CLOCK_SET_LEN);
    if (clockTickSem == NULL || clockSet == NULL) {
        ESP_LOGE(TAG, "clock queue set creation failed (out of memory)");
        return ESP_ERR_NO_MEM;
    }
    if (xQueueAddToSet(rtcCmdQueue, clockSet) != pdPASS) {
        ESP_LOGE(TAG, "cannot add rtcCmdQueue to clockSet");
        return ESP_ERR_NO_MEM;
    }
    if (xQueueAddToSet(clockTickSem, clockSet) != pdPASS) {
        ESP_LOGE(TAG, "cannot add clockTickSem to clockSet");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "queues created: event=%d display=%d alarm=%d rtcCmd=%d clockSet=%d",
             EVENT_QUEUE_LEN, DISPLAY_QUEUE_LEN, ALARM_QUEUE_LEN, RTC_CMD_QUEUE_LEN, CLOCK_SET_LEN);
    return ESP_OK;
}

bool app_event_send(const app_event_t *evt, const char *who)
{
    if (xQueueSend(eventQueue, evt, 0) == pdTRUE) {
        return true;
    }

    portENTER_CRITICAL(&s_drop_mux);
    s_drop_count++;
    uint32_t n = s_drop_count;
    portEXIT_CRITICAL(&s_drop_mux);

    if (n == 1U || (n % 50U) == 0U) {
        ESP_LOGW(TAG, "eventQueue full: %s dropped event type %d (total dropped %lu)",
                 who, (int)evt->type, (unsigned long)n);
    }
    return false;
}

uint32_t app_event_drop_count(void)
{
    portENTER_CRITICAL(&s_drop_mux);
    uint32_t n = s_drop_count;
    portEXIT_CRITICAL(&s_drop_mux);
    return n;
}
