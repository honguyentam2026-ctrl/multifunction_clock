#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "rtc_time.h"

esp_err_t storage_init(void);
esp_err_t storage_load_alarm(uint8_t *hour, uint8_t *minute, bool *enabled);
esp_err_t storage_save_alarm(uint8_t hour, uint8_t minute, bool enabled);
esp_err_t storage_load_cd_total(uint32_t *total_s);
esp_err_t storage_save_cd_total(uint32_t total_s);

/* Giờ lưu gần nhất. Trả ESP_ERR_NVS_NOT_FOUND nếu chưa có hoặc dữ liệu không hợp lệ. */
esp_err_t storage_load_time(rtc_time_t *t);
esp_err_t storage_save_time(const rtc_time_t *t);
