/*
 * dht - Driver DHT11/DHT22 bit-bang 1 dây (không dùng thư viện Arduino).
 *
 * LƯU Ý QUAN TRỌNG:
 *  - dht_read() tắt ngắt khoảng 4-5 ms khi lấy mẫu -> NGƯỜI GỌI phải giữ i2c_mutex
 *    trong suốt lời gọi để không có giao dịch I2C nào bị cắt ngang.
 *  - Không gọi từ ISR. ButtonTask có thể bị trễ tối đa ~5 ms (chấp nhận được).
 *  - Chu kỳ đọc tối thiểu: DHT11 = 1 s, DHT22 = 2 s. Gọi sớm hơn -> ESP_ERR_INVALID_STATE.
 */
#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

typedef enum { DHT_MODEL_DHT11, DHT_MODEL_DHT22 } dht_type_t;

/* Cấu hình chân (open-drain + pull-up nội dự phòng). Lần đọc đầu tiên chỉ được phép sau ~1.5 s. */
esp_err_t dht_init(gpio_num_t pin, dht_type_t type);

/* ESP_OK / ESP_ERR_TIMEOUT / ESP_ERR_INVALID_CRC / ESP_ERR_INVALID_STATE (gọi quá sớm)
 * / ESP_ERR_INVALID_RESPONSE (giá trị ngoài khoảng hợp lệ) */
esp_err_t dht_read(float *temp_c, float *humidity_pct);