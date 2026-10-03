/*
 * ModeManagerTask - consumer duy nhất của eventQueue; giữ trạng thái giao diện và gửi snapshot cho DisplayTask.
 * Phase 5: chỉ có chế độ CLOCK (xem giờ + chỉnh giờ bằng nút).
 */
#pragma once

#include "esp_err.h"

/* Tạo ModeManagerTask. Gọi sau app_queues_create(), TRƯỚC khi tạo các task producer. */
esp_err_t task_mode_start(void);
