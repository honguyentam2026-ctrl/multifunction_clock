/*
 * app_config.h - TẤT CẢ cấu hình phần cứng/thời gian/RTOS nằm ở đây.
 * Muốn đổi chân GPIO, độ ưu tiên, kích thước stack... chỉ cần sửa file này.
 */
#pragma once

#include "driver/gpio.h"

/* ====================== CHÂN GPIO (ESP32-C3) ====================== */
/* I2C dùng chung cho OLED + DS3231 */
#define I2C_SDA_GPIO        GPIO_NUM_6
#define I2C_SCL_GPIO        GPIO_NUM_7

/* 5 nút nhấn (active-low, bật pull-up nội) */
#define BTN_UP_GPIO         GPIO_NUM_1
#define BTN_DOWN_GPIO       GPIO_NUM_3
#define BTN_LEFT_GPIO       GPIO_NUM_4
#define BTN_RIGHT_GPIO      GPIO_NUM_5
#define BTN_CENTER_GPIO     GPIO_NUM_10

/* Còi active (active-high, qua transistor NPN) */
#define BUZZER_GPIO         GPIO_NUM_0

/* DHT11/DHT22: 1 dây DATA, cần pull-up 4.7-10k lên 3.3V (GPIO2 là chân strapping) */
#define DHT_GPIO            GPIO_NUM_2

/* LED active-LOW: 3.3V -> 330R -> anode, cathode -> GPIO8 (GPIO8 là chân strapping) */
#define LED_GPIO            GPIO_NUM_8
#define LED_ACTIVE_LEVEL    0

/* Tránh dùng: GPIO9 (BOOT), GPIO18/19 (USB), GPIO20/21 (UART0 console) */

/* ====================== I2C ====================== */
#define I2C_FREQ_HZ         400000
#define I2C_MUTEX_TIMEOUT_MS 100
#define OLED_I2C_ADDR       0x3C        /* đổi sang 0x3D nếu module của bạn dùng địa chỉ này */
#define DS3231_I2C_ADDR     0x68
/* #define OLED_DRIVER_SH1106 */        /* bỏ comment nếu màn hình là SH1106 */

/* ====================== CẢM BIẾN DHT ====================== */
// #define DHT_TYPE_DHT22
#define DHT_TYPE_DHT11

/* ====================== TÙY CHỌN ====================== */
#define ENABLE_DIAG_LOG     1           /* in stack/queue/heap mỗi 30 s */
#define ENABLE_BUTTON_BEEP  1           /* bíp ngắn khi bấm nút */
#define TEMP_WARN_C         35.0f       /* ngưỡng cảnh báo nhiệt độ cao */
#define TEMP_WARN_HYST_C    2.0f        /* chỉ cảnh báo lại sau khi nhiệt độ < TEMP_WARN_C - giá trị này */
#define DIAG_PERIOD_MS      30000       /* chu kỳ in chẩn đoán */
#define TIME_SAVE_PERIOD_SEC 10           /* lưu giờ NVS mỗi N giây, khi giây chia hết cho N */

/* ====================== TASK: ĐỘ ƯU TIÊN / STACK ====================== */
/* Ưu tiên: nút cao nhất (không mất phím) > clock/mode/alarm (nhạy thời gian)
 * > display (trễ vài chục ms không sao) > temp (đổi rất chậm). */
#define PRIO_BUTTON         4
#define PRIO_CLOCK          3
#define PRIO_MODE_MANAGER   3
#define PRIO_ALARM          3
#define PRIO_DISPLAY        2
#define PRIO_TEMP           1
#define PRIO_DIAG           1

#define STACK_BUTTON        3072
#define STACK_CLOCK         4096
#define STACK_MODE_MANAGER  5120
#define STACK_ALARM         2048
#define STACK_DISPLAY       4096
#define STACK_TEMP          3072
#define STACK_DIAG          3072

/* ====================== QUEUE ====================== */
#define EVENT_QUEUE_LEN     16
#define DISPLAY_QUEUE_LEN   1
#define ALARM_QUEUE_LEN     4
#define RTC_CMD_QUEUE_LEN   2

/* ====================== CHU KỲ (ms) ====================== */
#define BUTTON_PERIOD_MS    10
#define CLOCK_PERIOD_MS     100
#define TEMP_PERIOD_MS      2000
#define DISPLAY_TIMEOUT_MS  250