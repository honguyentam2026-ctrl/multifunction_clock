/*
 * task_clock.c - ClockTask
 *   Mục đích : là task DUY NHẤT nói chuyện với DS3231 (đọc giờ, ghi giờ). Khi DS3231 lỗi
 *              thì tự chạy ĐỒNG HỒ PHẦN MỀM (tính từ esp_timer) để sản phẩm vẫn hoạt động.
 *   Độ ưu tiên: PRIO_CLOCK (3)
 *   Chu kỳ   : CLOCK_PERIOD_MS (100 ms), nhận nhịp từ clockTickSem qua clockSet
 *   Queue    : NHẬN rtcCmdQueue qua clockSet -> ghi giờ vào DS3231 (hoặc vào đồng hồ mềm)
 *              GỬI  eventQueue  (timeout 0)  -> EVT_TICK_100MS mỗi chu kỳ,
 *                                               EVT_TIME mỗi khi giây đổi (soft_time = true nếu là đồng hồ mềm),
 *                                               EVT_RTC_ERROR một lần khi chuyển sang đồng hồ mềm
 *   I2C      : giữ i2c_bus_lock() quanh MỖI lần gọi ds3231_*
 *
 * Phase 8: đồng hồ phần mềm.
 *   - DS3231 đọc lỗi 3 lần liên tiếp (300 ms) -> gửi EVT_RTC_ERROR, rồi tiếp tục phát EVT_TIME
 *     mỗi giây từ esp_timer, bắt đầu từ giờ hợp lệ gần nhất.
 *   - Không phát hiện DS3231 lúc khởi động -> bắt đầu từ giờ tốt nhất (NVS hoặc giờ build).
 *   - Trong lúc lỗi, mỗi giây thử đọc lại DS3231; đọc được thì tự quay về giờ thật (không cần reset).
 *   - Người dùng chỉnh giờ khi đang ở đồng hồ mềm: giờ mới được áp dụng vào đồng hồ mềm.
 */
#include "task_clock.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "app_config.h"
#include "app_types.h"
#include "app_queues.h"
#include "i2c_bus.h"
#include "ds3231.h"
#include "storage.h"

static const char *TAG = "CLOCK";

#define CLOCK_FAIL_LIMIT   3                 /* đọc lỗi 3 lần liên tiếp (300 ms) mới coi là mất RTC */
#define RTC_RETRY_US       1000000LL         /* đang lỗi: thử đọc lại mỗi 1 s */
#define RTC_REINIT_US      10000000LL        /* chưa từng init được: thử init lại mỗi 10 s */

/* Chỉ ghi MỘT lần trong task_clock_start() trước khi tạo task, sau đó chỉ ClockTask đọc. */
static bool s_rtc_init_ok = false;
static int64_t s_last_saved_second = -1;
static TimerHandle_t s_tick_timer = NULL;

typedef struct {
    rtc_time_t base;        /* giờ tại thời điểm base_us */
    int64_t    base_us;     /* mốc esp_timer tương ứng */
    bool       valid;       /* đã có giờ để bắt đầu */
} soft_clock_t;

/* ============================ tiện ích thời gian ============================ */

/* Giờ lúc build firmware, dùng làm giờ khởi đầu khi không có DS3231. */
static void build_time(rtc_time_t *t)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = "Jan";
    int d = 1, y = 2025, hh = 0, mm = 0, ss = 0;

    (void)sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
    (void)sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);

    const char *p = strstr(months, mon);
    int m = (p != NULL) ? (int)((p - months) / 3) + 1 : 1;

    memset(t, 0, sizeof(*t));
    t->year = (uint16_t)y;
    t->month = (uint8_t)m;
    t->day = (uint8_t)d;
    t->hour = (uint8_t)hh;
    t->minute = (uint8_t)mm;
    t->second = (uint8_t)ss;
    t->weekday = ds3231_calc_weekday(t->year, t->month, t->day);
}

static int time_cmp(const rtc_time_t *a, const rtc_time_t *b)
{
    if (a->year != b->year) return (a->year > b->year) ? 1 : -1;
    if (a->month != b->month) return (a->month > b->month) ? 1 : -1;
    if (a->day != b->day) return (a->day > b->day) ? 1 : -1;
    if (a->hour != b->hour) return (a->hour > b->hour) ? 1 : -1;
    if (a->minute != b->minute) return (a->minute > b->minute) ? 1 : -1;
    if (a->second != b->second) return (a->second > b->second) ? 1 : -1;
    return 0;
}

static int64_t time_second_key(const rtc_time_t *t)
{
    int64_t key = t->year;
    key = key * 12 + t->month;
    key = key * 31 + t->day;
    key = key * 24 + t->hour;
    key = key * 60 + t->minute;
    return key * 60 + t->second;
}

static void best_estimate_time(rtc_time_t *out)
{
    rtc_time_t built;
    build_time(&built);
    rtc_time_t saved;
    esp_err_t err = storage_load_time(&saved);
    if (err == ESP_OK && time_cmp(&saved, &built) > 0) {
        *out = saved;
        ESP_LOGW(TAG, "using saved NVS time %04d-%02d-%02d %02d:%02d:%02d (newer than build time)",
                 (int)saved.year, (int)saved.month, (int)saved.day,
                 (int)saved.hour, (int)saved.minute, (int)saved.second);
    } else {
        *out = built;
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "cannot load saved time (%s), using build time", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "using build time %04d-%02d-%02d %02d:%02d:%02d",
                     (int)built.year, (int)built.month, (int)built.day,
                     (int)built.hour, (int)built.minute, (int)built.second);
        }
    }
}

/* Cộng n giây vào t, xử lý tràn phút/giờ/ngày/tháng/năm (có năm nhuận). Năm giới hạn 2000..2099 như DS3231. */
static void time_add_seconds(rtc_time_t *t, uint32_t n)
{
    uint32_t total = (uint32_t)t->second + n;
    t->second = (uint8_t)(total % 60U);
    total /= 60U;
    total += t->minute;
    t->minute = (uint8_t)(total % 60U);
    total /= 60U;
    total += t->hour;
    t->hour = (uint8_t)(total % 24U);
    total /= 24U;                                    /* còn lại: số NGÀY cần cộng */

    while (total > 0U) {
        t->day++;
        if (t->day > ds3231_days_in_month(t->year, t->month)) {
            t->day = 1;
            t->month++;
            if (t->month > 12) {
                t->month = 1;
                t->year = (t->year < 2099) ? (uint16_t)(t->year + 1U) : 2000U;
            }
        }
        total--;
    }
    t->weekday = ds3231_calc_weekday(t->year, t->month, t->day);
}

/* ============================ gửi sự kiện / truy cập RTC ============================ */

static void send_time(const rtc_time_t *t, bool soft)
{
    app_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = EVT_TIME;
    evt.data.time = *t;
    evt.soft_time = soft;
    (void)app_event_send(&evt, "ClockTask");
}

static void send_simple(event_type_t type)
{
    app_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    (void)app_event_send(&evt, "ClockTask");
}

static void tick_timer_cb(TimerHandle_t t)
{
    (void)t;
    (void)xSemaphoreGive(clockTickSem);
}

static esp_err_t rtc_read_locked(rtc_time_t *t)
{
    if (!i2c_bus_lock(I2C_MUTEX_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ds3231_get_time(t);
    i2c_bus_unlock();
    return err;
}

static esp_err_t rtc_write_locked(const rtc_time_t *t)
{
    if (!i2c_bus_lock(I2C_MUTEX_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ds3231_set_time(t);
    i2c_bus_unlock();
    return err;
}

static esp_err_t rtc_init_locked(void)
{
    if (!i2c_bus_lock(I2C_MUTEX_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ds3231_init();
    i2c_bus_unlock();
    return err;
}

/* Khôi phục giờ sau khi init phát hiện OSF; gọi chỉ sau rtc_init_locked() thành công. */
static void recover_after_power_loss(void)
{
    if (ds3231_power_lost_at_init()) {
        rtc_time_t t;
        best_estimate_time(&t);
        esp_err_t err = rtc_write_locked(&t);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "RTC lost power -> restored %04d-%02d-%02d %02d:%02d:%02d (may be behind real time)",
                     (int)t.year, (int)t.month, (int)t.day,
                     (int)t.hour, (int)t.minute, (int)t.second);
        } else {
            ESP_LOGE(TAG, "RTC restore failed: %s", esp_err_to_name(err));
        }
        err = storage_save_time(&t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot save restored time: %s", esp_err_to_name(err));
        } else {
            s_last_saved_second = time_second_key(&t);
        }
    }
}

/* ============================ task ============================ */

static void clock_task(void *arg)
{
    (void)arg;
    int fail_count = 0;
    int last_second = -1;
    bool rtc_inited = s_rtc_init_ok;
    bool sw_mode = false;               /* true = DS3231 lỗi, đang chạy đồng hồ phần mềm */
    bool sw_send_now = false;           /* cần phát EVT_TIME mềm ngay (vừa vào chế độ mềm / vừa chỉnh giờ) */
    int64_t last_retry_us = 0;
    int64_t last_reinit_us = 0;
    soft_clock_t sw;
    memset(&sw, 0, sizeof(sw));

    if (!rtc_inited) {
        /* Không có DS3231 từ lúc khởi động: chạy đồng hồ mềm từ giờ tốt nhất có thể. */
        best_estimate_time(&sw.base);
        sw.base_us = esp_timer_get_time();
        sw.valid = true;
        sw_mode = true;
        sw_send_now = true;
        last_retry_us = sw.base_us;
        last_reinit_us = sw.base_us;
        ESP_LOGW(TAG, "RTC not detected at boot -> SOFTWARE clock from best available time");
        send_simple(EVT_RTC_ERROR);
    }

    while (1) {
        QueueSetMemberHandle_t member = xQueueSelectFromSet(clockSet, portMAX_DELAY);
        if (member == (QueueSetMemberHandle_t)rtcCmdQueue) {
            rtc_cmd_t cmd;
            if (xQueueReceive(rtcCmdQueue, &cmd, 0) == pdTRUE) {
                int64_t now = esp_timer_get_time();
                if (sw_mode) {
                    sw.base = cmd.time;
                    sw.base_us = now;
                    sw.valid = true;
                    sw_send_now = true;
                    esp_err_t save_err = storage_save_time(&cmd.time);
                    if (save_err != ESP_OK) {
                        ESP_LOGE(TAG, "cannot save manually set time: %s", esp_err_to_name(save_err));
                    } else {
                        s_last_saved_second = time_second_key(&cmd.time);
                    }
                    ESP_LOGI(TAG, "SOFTWARE clock set to %04d-%02d-%02d %02d:%02d:%02d",
                             (int)cmd.time.year, (int)cmd.time.month, (int)cmd.time.day,
                             (int)cmd.time.hour, (int)cmd.time.minute, (int)cmd.time.second);
                } else {
                    esp_err_t err = rtc_write_locked(&cmd.time);
                    if (err == ESP_OK) {
                        esp_err_t save_err = storage_save_time(&cmd.time);
                        if (save_err != ESP_OK) {
                            ESP_LOGE(TAG, "cannot save manually set time: %s", esp_err_to_name(save_err));
                        } else {
                            s_last_saved_second = time_second_key(&cmd.time);
                        }
                        ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d",
                                 (int)cmd.time.year, (int)cmd.time.month, (int)cmd.time.day,
                                 (int)cmd.time.hour, (int)cmd.time.minute, (int)cmd.time.second);
                        last_second = -1;
                    } else {
                        ESP_LOGE(TAG, "RTC write failed: %s", esp_err_to_name(err));
                    }
                }
            }
        } else if (member == (QueueSetMemberHandle_t)clockTickSem) {
            /* Semaphore nhị phân gộp tick bị lỡ nếu ClockTask đang bận, như cách bắt kịp tick cũ. */
            if (xSemaphoreTake(clockTickSem, 0) == pdTRUE) {
                int64_t now = esp_timer_get_time();

                /* Nhịp 100 ms phát mỗi lần được nhận, kể cả khi DS3231 lỗi. */
                send_simple(EVT_TICK_100MS);

                if (!sw_mode) {
                    /* ---- chế độ bình thường: đọc DS3231 ---- */
                    rtc_time_t t;
                    memset(&t, 0, sizeof(t));
                    esp_err_t err = rtc_read_locked(&t);
                    if (err == ESP_OK) {
                        fail_count = 0;
                        sw.base = t;                    /* luôn giữ giờ tốt gần nhất để làm điểm xuất phát */
                        sw.base_us = now;
                        sw.valid = true;
                        if (t.second != last_second) {
                            last_second = t.second;
                            send_time(&t, false);
                            if ((t.second % TIME_SAVE_PERIOD_SEC) == 0) {
                                int64_t second_key = time_second_key(&t);
                                if (second_key != s_last_saved_second) {
                                    s_last_saved_second = second_key;
                                    esp_err_t save_err = storage_save_time(&t);
                                    if (save_err != ESP_OK) {
                                        ESP_LOGE(TAG, "periodic time save failed: %s", esp_err_to_name(save_err));
                                    }
                                }
                            }
                        }
                    } else {
                        if (fail_count < CLOCK_FAIL_LIMIT) {
                            fail_count++;
                        }
                        if (fail_count >= CLOCK_FAIL_LIMIT) {
                            ESP_LOGE(TAG, "RTC read failed: %s -> switching to SOFTWARE clock", esp_err_to_name(err));
                            if (!sw.valid) {
                                best_estimate_time(&sw.base);
                                sw.base_us = now;
                                sw.valid = true;
                            }
                            sw_mode = true;
                            sw_send_now = true;
                            last_retry_us = now;
                            last_reinit_us = now;
                            send_simple(EVT_RTC_ERROR);
                        }
                    }
                } else {
                    /* ---- đồng hồ phần mềm: tiến theo esp_timer, giữ phần lẻ giây để không trôi ---- */
                    int64_t elapsed = now - sw.base_us;
                    if (elapsed >= 1000000LL) {
                        uint32_t n = (uint32_t)(elapsed / 1000000LL);
                        time_add_seconds(&sw.base, n);
                        sw.base_us += (int64_t)n * 1000000LL;
                        sw_send_now = true;
                    }
                    if (sw_send_now) {
                        sw_send_now = false;
                        send_time(&sw.base, true);
                    }

                    /* Mỗi giây thử đọc lại DS3231 để tự phục hồi. */
                    if (now - last_retry_us >= RTC_RETRY_US) {
                        last_retry_us = now;
                        if (!rtc_inited && (now - last_reinit_us) >= RTC_REINIT_US) {
                            last_reinit_us = now;
                            if (rtc_init_locked() == ESP_OK) {
                                rtc_inited = true;
                                recover_after_power_loss();
                            }
                        }
                        rtc_time_t t;
                        memset(&t, 0, sizeof(t));
                        if (rtc_inited && rtc_read_locked(&t) == ESP_OK) {
                            ESP_LOGI(TAG, "RTC recovered -> back to hardware time");
                            sw_mode = false;
                            fail_count = 0;
                            sw.base = t;
                            sw.base_us = now;
                            last_second = t.second;
                            send_time(&t, false);
                        }
                    }
                }
            }
        }
    }
}

esp_err_t task_clock_start(void)
{
    /* DS3231 không phát hiện KHÔNG phải lỗi chết người: vẫn tạo task và chạy đồng hồ mềm. */
    esp_err_t err = rtc_init_locked();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ds3231_init failed (%s) - will run software clock", esp_err_to_name(err));
        s_rtc_init_ok = false;
    } else {
        s_rtc_init_ok = true;
        recover_after_power_loss();
    }

    if (xTaskCreate(clock_task, "ClockTask", STACK_CLOCK, NULL, PRIO_CLOCK, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create ClockTask");
        return ESP_ERR_NO_MEM;
    }

    s_tick_timer = xTimerCreate("ClockTick", pdMS_TO_TICKS(CLOCK_PERIOD_MS),
                                pdTRUE, NULL, tick_timer_cb);
    if (s_tick_timer == NULL) {
        ESP_LOGE(TAG, "cannot create ClockTick timer");
        return ESP_ERR_NO_MEM;
    }
    if (xTimerStart(s_tick_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "cannot start ClockTick timer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ClockTask started (tick %d ms)", CLOCK_PERIOD_MS);
    return ESP_OK;
}
