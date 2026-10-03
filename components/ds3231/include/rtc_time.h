/*
 * rtc_time.h - Kiểu thời gian dùng chung (driver ds3231 và app_types.h cùng dùng).
 * Đặt trong component ds3231 vì component không thể include file nằm trong main/.
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint8_t  hour, minute, second;   /* định dạng 24h */
    uint8_t  day, month;             /* 1..31, 1..12 */
    uint16_t year;                   /* ví dụ 2026 */
    uint8_t  weekday;                /* 0=Thứ 2 .. 6=Chủ nhật (tự tính, không tin thanh ghi RTC) */
} rtc_time_t;