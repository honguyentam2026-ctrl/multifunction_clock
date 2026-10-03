/*
 * DisplayTask - nhận snapshot display_msg_t từ displayQueue và vẽ lên OLED 128x64.
 */
#pragma once

#include "esp_err.h"

/* Khởi tạo OLED (bus I2C phải sẵn sàng), vẽ màn hình chào và tạo DisplayTask. */
esp_err_t task_display_start(void);
