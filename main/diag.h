#pragma once

#include "esp_err.h"

/* Bật DiagTask (in task table 1 lần + stack/queue/heap mỗi DIAG_PERIOD_MS).
 * Nếu ENABLE_DIAG_LOG = 0 thì hàm này không làm gì và trả về ESP_OK. */
esp_err_t diag_start(void);
