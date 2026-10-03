/*
 * diag.c - DiagTask (chẩn đoán, phục vụ báo cáo)
 *   Mục đích : 1 lần sau khi khởi động in bảng task (vTaskList); sau đó mỗi 30 s in
 *              stack còn trống của từng task, số phần tử đang chờ trong từng queue, heap, số sự kiện bị rơi.
 *   Ưu tiên  : PRIO_DIAG (1) - thấp nhất, không ảnh hưởng các task thời gian thực.
 *   Chu kỳ   : DIAG_PERIOD_MS, dùng vTaskDelayUntil
 *   Queue    : chỉ ĐỌC số phần tử (uxQueueMessagesWaiting), không nhận/gửi gì.
 */
#include "diag.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "app_config.h"
#include "app_queues.h"

#if ENABLE_DIAG_LOG

static const char *TAG = "DIAG";

typedef struct { const char *name; uint32_t stack_bytes; } diag_task_t;

/* Tên phải khớp tên truyền vào xTaskCreate ở từng task. */
static const diag_task_t TASKS[] = {
    { "ButtonTask",      STACK_BUTTON },
    { "ClockTask",       STACK_CLOCK },
    { "ModeManagerTask", STACK_MODE_MANAGER },
    { "AlarmTask",       STACK_ALARM },
    { "DisplayTask",     STACK_DISPLAY },
    { "TempTask",        STACK_TEMP },
    { "DiagTask",        STACK_DIAG },
};

#define LOW_STACK_PERCENT 25U               /* còn trống dưới 25% thì cảnh báo */

#if (configUSE_TRACE_FACILITY == 1) && (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
static void log_task_table(void)
{
    static char buf[1024];                   /* static: không tốn stack của task */
    vTaskList(buf);
    ESP_LOGI(TAG, "Task table (Name State Prio StackHighWaterMark Num):\n%s", buf);
}
#else
static void log_task_table(void)
{
    ESP_LOGW(TAG, "vTaskList unavailable (enable FREERTOS_USE_TRACE_FACILITY + STATS_FORMATTING_FUNCTIONS)");
}
#endif

static void log_stacks(void)
{
    for (size_t i = 0; i < sizeof(TASKS) / sizeof(TASKS[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(TASKS[i].name);
        if (h == NULL) {
            ESP_LOGW(TAG, "%-16s not found", TASKS[i].name);
            continue;
        }
        uint32_t free_b = (uint32_t)uxTaskGetStackHighWaterMark(h);   /* ESP-IDF: đơn vị byte */
        uint32_t pct = (TASKS[i].stack_bytes > 0U) ? (free_b * 100U) / TASKS[i].stack_bytes : 0U;
        if (pct < LOW_STACK_PERCENT) {
            ESP_LOGW(TAG, "%-16s stack free %4lu / %4lu B (%2lu%%)  <-- LOW (<%u%%)", TASKS[i].name,
                     (unsigned long)free_b, (unsigned long)TASKS[i].stack_bytes, (unsigned long)pct,
                     LOW_STACK_PERCENT);
        } else {
            ESP_LOGI(TAG, "%-16s stack free %4lu / %4lu B (%2lu%%)", TASKS[i].name,
                     (unsigned long)free_b, (unsigned long)TASKS[i].stack_bytes, (unsigned long)pct);
        }
    }
}

static void log_queues(void)
{
    ESP_LOGI(TAG, "queues waiting: event %u/%d  display %u/%d  alarm %u/%d  rtcCmd %u/%d  (events dropped: %lu)",
             (unsigned)uxQueueMessagesWaiting(eventQueue), EVENT_QUEUE_LEN,
             (unsigned)uxQueueMessagesWaiting(displayQueue), DISPLAY_QUEUE_LEN,
             (unsigned)uxQueueMessagesWaiting(alarmQueue), ALARM_QUEUE_LEN,
             (unsigned)uxQueueMessagesWaiting(rtcCmdQueue), RTC_CMD_QUEUE_LEN,
             (unsigned long)app_event_drop_count());
}

static void diag_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));        /* chờ các task khác khởi động xong */
    log_task_table();

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        log_stacks();
        log_queues();
        ESP_LOGI(TAG, "heap free %lu B, minimum ever %lu B",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DIAG_PERIOD_MS));
    }
}

esp_err_t diag_start(void)
{
    if (xTaskCreate(diag_task, "DiagTask", STACK_DIAG, NULL, PRIO_DIAG, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create DiagTask");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "DiagTask started (every %d ms)", DIAG_PERIOD_MS);
    return ESP_OK;
}

#else  /* ENABLE_DIAG_LOG == 0 */

esp_err_t diag_start(void)
{
    return ESP_OK;
}

#endif
