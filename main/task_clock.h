/*
 * ClockTask - đọc DS3231 mỗi 100 ms, phát EVT_TIME khi giây đổi, nhận lệnh ghi giờ từ rtcCmdQueue.
 */
#pragma once

#include "esp_err.h"

/* Khởi tạo DS3231 (bus I2C phải sẵn sàng) và tạo ClockTask. Gọi sau app_queues_create(). */
esp_err_t task_clock_start(void);
