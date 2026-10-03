/*
 * app_types.h - Mọi kiểu dữ liệu dùng chung giữa các task (sự kiện, snapshot hiển thị, lệnh).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "rtc_time.h"

typedef enum { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_CENTER, BTN_COUNT } button_id_t;

typedef enum {
    PRESS_SHORT,    /* nhả ra trước ngưỡng nhấn giữ */
    PRESS_LONG,     /* CHỈ BTN_CENTER: phát 1 lần khi chạm ngưỡng */
    PRESS_REPEAT    /* CHỈ UP/DOWN/LEFT/RIGHT: tự lặp khi giữ */
} press_type_t;

/* rtc_time_t được định nghĩa trong components/ds3231/include/rtc_time.h */

typedef enum { EVT_BUTTON, EVT_TICK_100MS, EVT_TIME, EVT_ENV, EVT_RTC_ERROR } event_type_t;

typedef struct {
    event_type_t type;
    union {
        struct { button_id_t id; press_type_t press; } btn;
        rtc_time_t time;
        struct { float temp_c; float humidity_pct; bool humidity_valid; bool from_rtc; } env;
    } data;
    bool soft_time;      /* EVT_TIME: true = giờ do đồng hồ phần mềm tạo ra (DS3231 lỗi) */
} app_event_t;

typedef enum { UI_CLOCK, UI_ALARM, UI_STOPWATCH, UI_COUNTDOWN, UI_TEMP, UI_MODE_COUNT } ui_mode_t;
typedef enum { CD_IDLE, CD_RUNNING, CD_PAUSED, CD_DONE } countdown_state_t;

typedef struct {
    ui_mode_t mode;
    bool      editing;           /* đang ở chế độ chỉnh sửa */
    uint8_t   edit_field;        /* trường đang nhấp nháy */
    bool      alarm_enabled;
    bool      alarm_ringing;     /* overlay báo thức toàn màn hình */
    bool      snooze_active;     /* báo lại sau 5 phút */
    bool      timer_done;        /* overlay "TIME UP" */
    bool      rtc_ok;
    rtc_time_t time;             /* giờ hiện tại (hoặc giờ đang chỉnh) */
    float     temp_c;  float temp_min;  float temp_max;
    float     hum_pct; float hum_min;   float hum_max;
    bool      env_valid;         /* false cho tới khi có số đo hợp lệ đầu tiên */
    bool      hum_valid;         /* false khi DHT lỗi (DS3231 không có độ ẩm) */
    bool      temp_from_rtc;     /* true khi nhiệt độ lấy từ DS3231 (dự phòng) */
    bool      temp_warn;         /* đang trong trạng thái cảnh báo nhiệt độ cao */
    uint8_t   alarm_hour, alarm_minute;
    struct { uint32_t elapsed_ms; bool running; uint32_t laps_ms[5]; uint8_t lap_count; } sw;
    struct { uint32_t remain_ms; uint32_t total_ms; countdown_state_t state; } cd;
} display_msg_t;

typedef enum { ALARM_CMD_STOP, ALARM_CMD_BEEP_SHORT, ALARM_CMD_RING_ALARM,
               ALARM_CMD_RING_TIMER, ALARM_CMD_TEMP_WARN, ALARM_CMD_LED_STEADY_ON,
               ALARM_CMD_LED_STEADY_OFF } alarm_cmd_t;

typedef struct { rtc_time_t time; } rtc_cmd_t;   /* yêu cầu: ghi giờ này vào DS3231 */