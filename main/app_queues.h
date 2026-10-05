/*
 * app_queues - Các queue dùng chung giữa các task.
 *
 * eventQueue   (16 x app_event_t)   Button, Clock, Temp  -> ModeManager
 * displayQueue (1  x display_msg_t) ModeManager          -> Display
 * alarmQueue   (4  x alarm_cmd_t)   ModeManager          -> Alarm
 * rtcCmdQueue  (2  x rtc_cmd_t)     ModeManager          -> Clock
 * clockSet gồm rtcCmdQueue và clockTickSem, chỉ ClockTask chờ trên set này.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "app_types.h"

extern QueueHandle_t eventQueue;
extern QueueHandle_t displayQueue;
extern QueueHandle_t alarmQueue;
extern QueueHandle_t rtcCmdQueue;
extern QueueSetHandle_t clockSet;
extern SemaphoreHandle_t clockTickSem;

esp_err_t app_queues_create(void);

/* Gửi sự kiện không chặn; queue đầy thì đếm sự kiện rơi và log có giới hạn tần suất. */
bool app_event_send(const app_event_t *evt, const char *who);
uint32_t app_event_drop_count(void);
