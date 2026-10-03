/*
 * i2c_bus - Khởi tạo bus I2C master (driver MỚI) + mutex dùng chung cho OLED và DS3231.
 *
 * Quy ước: i2c_bus KHÔNG tự khóa mutex bên trong các hàm của nó (trừ khi ghi chú khác).
 * Task nào truyền dữ liệu qua I2C phải gọi i2c_bus_lock() / i2c_bus_unlock() quanh giao dịch.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

/* Tạo bus (I2C_NUM_0, 400 kHz, bật pull-up nội dự phòng) và mutex. Gọi 1 lần ở app_main. */
esp_err_t i2c_bus_init(gpio_num_t sda, gpio_num_t scl);

/* Lấy handle bus để driver thiết bị (ds3231, ssd1306) gắn thêm device. */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/* Khóa/mở khóa mutex bus. lock trả về false nếu hết thời gian chờ (không bao giờ chờ vô hạn). */
bool i2c_bus_lock(uint32_t timeout_ms);
void i2c_bus_unlock(void);

/* Dò 1 địa chỉ: ESP_OK nếu có thiết bị trả lời. Hàm này tự khóa mutex. */
esp_err_t i2c_bus_probe(uint8_t addr);

/* Quét toàn bộ 0x08..0x77 và log các địa chỉ tìm thấy. Trả về số thiết bị. Tự khóa mutex. */
int i2c_bus_scan(void);