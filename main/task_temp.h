/*
 * TempTask - đọc DHT mỗi 2 s, gửi EVT_ENV; DHT lỗi thì dự phòng bằng nhiệt độ của DS3231.
 */
#pragma once

#include "esp_err.h"

/* Cấu hình chân DHT và tạo TempTask. Gọi sau app_queues_create() và sau khi bus I2C đã khởi tạo. */
esp_err_t task_temp_start(void);
