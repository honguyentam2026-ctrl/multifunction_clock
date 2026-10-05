/*
 * ds3231 - Driver RTC DS3231 (địa chỉ I2C 0x68) trên driver I2C master mới.
 *
 * QUY ƯỚC KHÓA BUS: các hàm ở đây KHÔNG tự khóa i2c_mutex.
 * NGƯỜI GỌI phải gọi i2c_bus_lock() trước và i2c_bus_unlock() sau mỗi lần gọi.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "rtc_time.h"

/* Gắn thiết bị vào bus, kiểm tra cờ OSF (mất nguồn pin) -> nếu có thì đặt giờ = giờ build
 * rồi xóa OSF; ép chế độ 24h. Gọi khi đang giữ i2c_mutex. Trạng thái OSF lúc khởi tạo
 * được giữ lại để truy vấn bằng ds3231_power_lost_at_init(). */
esp_err_t ds3231_init(void);

/* Đọc ngày giờ (weekday được tự tính từ ngày). */
esp_err_t ds3231_get_time(rtc_time_t *t);

/* Ghi ngày giờ (kiểm tra hợp lệ: năm 2000..2099, tháng, ngày theo năm nhuận, giờ/phút/giây). */
esp_err_t ds3231_set_time(const rtc_time_t *t);

/* Nhiệt độ nội của DS3231 (độ phân giải 0.25 độ C) - chỉ dùng làm dự phòng khi DHT lỗi. */
esp_err_t ds3231_get_temperature(float *temp_c);

/* true nếu cờ OSF đang bật (dao động từng bị dừng, giờ không đáng tin). */
bool ds3231_lost_power(void);

/* true nếu lần ds3231_init() gần nhất phát hiện OSF (RTC từng mất nguồn). Không đọc bus. */
bool ds3231_power_lost_at_init(void);

/* Tính thứ trong tuần (Sakamoto). Trả về 0=Thứ 2 .. 6=Chủ nhật. */
uint8_t ds3231_calc_weekday(uint16_t year, uint8_t month, uint8_t day);

/* Số ngày trong tháng (có xử lý năm nhuận). */
uint8_t ds3231_days_in_month(uint16_t year, uint8_t month);