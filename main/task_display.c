/*
 * task_display.c - DisplayTask
 *   Mục đích : task DUY NHẤT chạm vào OLED; chỉ VẼ snapshot nhận được, không quyết định gì.
 *   Ưu tiên  : PRIO_DISPLAY (2)
 *   Chu kỳ   : chặn trên displayQueue, timeout DISPLAY_TIMEOUT_MS (dùng cho nhấp nháy)
 *   Queue    : NHẬN displayQueue (ModeManagerTask gửi bằng xQueueOverwrite)
 *   I2C      : giữ i2c_mutex quanh ssd1306_set_contrast() và ssd1306_flush()
 */
#include "task_display.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_types.h"
#include "app_queues.h"
#include "i2c_bus.h"
#include "ssd1306.h"
#include "font5x7.h"

static const char *TAG = "DISP";
#define BLINK_PERIOD_MS 500
#define MODE_DOT_START_X (SSD1306_WIDTH - UI_MODE_COUNT * 4)
#define MODE_FOOTER_WIDTH (MODE_DOT_START_X - 2)

static const char *const WEEKDAY_NAME[7] = { "T2", "T3", "T4", "T5", "T6", "T7", "CN" };

typedef struct { int x, y, w, h; } rect_t;
static const rect_t EDIT_RECT[5] = {
    {  3, 13, 35, 23 },
    { 57, 13, 35, 23 },
    {  4, 39, 24, 16 },
    { 40, 39, 24, 16 },
    { 76, 39, 48, 16 },
};
static const rect_t EDIT_RECT_ALARM[2] = {
    {  6,  8, 48, 32 },
    { 78,  8, 48, 32 },
};

static void draw_mode_dots(const display_msg_t *m)
{
    int start_x = MODE_DOT_START_X;
    int y = 58;
    for (int i = 0; i < UI_MODE_COUNT; i++) {
        bool filled = (i == (int)m->mode);
        int x = start_x + i * 4;
        ssd1306_draw_rect(x, y, 2, 2, filled);
    }
}

static void draw_text_clipped(int x, int y, const char *text, int scale, int max_width)
{
    char clipped[48];
    size_t len = strlen(text);
    size_t max_chars = (size_t)((max_width + scale) / (6 * scale));
    if (len > max_chars) {
        len = max_chars;
    }
    if (len >= sizeof(clipped)) {
        len = sizeof(clipped) - 1U;
    }
    memcpy(clipped, text, len);
    clipped[len] = '\0';
    ssd1306_draw_text_scaled(x, y, clipped, scale);
}

static float display_temperature(float celsius, bool unit_f)
{
    return unit_f ? (celsius * 9.0f / 5.0f) + 32.0f : celsius;
}

static char temperature_unit(const display_msg_t *m)
{
    return m->settings.unit_f ? 'F' : 'C';
}

static void draw_clock_screen(const display_msg_t *m, bool highlight)
{
    char buf[32];
    ssd1306_clear();

    const char *title = (m->editing) ? "EDIT" : (m->alarm_ringing ? "ALARM" : ((m->mode == UI_CLOCK) ? "CLOCK" : ((m->mode == UI_ALARM) ? "ALARM" : "MODE")));
    ssd1306_draw_text_scaled(0, 0, title, 1);
    if (m->time.weekday < 7) {
        const char *w = WEEKDAY_NAME[m->time.weekday];
        ssd1306_draw_text_scaled(SSD1306_WIDTH - ssd1306_text_width(w, 1), 0, w, 1);
    }

    if (!m->rtc_ok) {
        ssd1306_draw_text_scaled(40, 0, "RTC ERR", 1);
    }
    if (m->alarm_enabled) {
        ssd1306_draw_bitmap(104, 0, 8, 8, icon_bell);
    }

    snprintf(buf, sizeof(buf), "%02d:%02d", (int)m->time.hour, (int)m->time.minute);
    ssd1306_draw_text_scaled(4, 14, buf, 3);
    snprintf(buf, sizeof(buf), "%02d", (int)m->time.second);
    ssd1306_draw_text_scaled(96, 21, buf, 2);

    snprintf(buf, sizeof(buf), "%02d/%02d/%04d", (int)m->time.day, (int)m->time.month, (int)m->time.year);
    ssd1306_draw_text_scaled(5, 40, buf, 2);

    if (m->alarm_ringing) {
        ssd1306_draw_text_scaled(0, 56, "ALARM!", 1);
        if (highlight) {
            ssd1306_invert_region(0, 56, 40, 8);
        }
    } else if (m->editing) {
        draw_text_clipped(0, 56, "OK:save HOLD:cncl", 1, MODE_FOOTER_WIDTH);
        if (highlight && m->edit_field < 5) {
            const rect_t *r = &EDIT_RECT[m->edit_field];
            ssd1306_invert_region(r->x, r->y, r->w, r->h);
        }
    } else if (m->mode == UI_ALARM) {
        snprintf(buf, sizeof(buf), "STATE: %s", m->alarm_enabled ? "ON" : "OFF");
        ssd1306_draw_text_scaled(0, 56, buf, 1);
    } else if (!m->rtc_ok) {
        draw_text_clipped(0, 56, "RTC ERROR-SW", 1, MODE_FOOTER_WIDTH);
    } else if (!m->env_valid) {
        (void)snprintf(buf, sizeof(buf), "--.-%c", temperature_unit(m));
        draw_text_clipped(0, 56, buf, 1, MODE_FOOTER_WIDTH);
    } else if (m->hum_valid) {
        (void)snprintf(buf, sizeof(buf), "%.1f%c H:%d%%",
                       (double)display_temperature(m->temp_c, m->settings.unit_f),
                       temperature_unit(m), (int)(m->hum_pct + 0.5f));
        draw_text_clipped(0, 56, buf, 1, MODE_FOOTER_WIDTH);
    } else {
        (void)snprintf(buf, sizeof(buf), "%.1f%c %s",
                       (double)display_temperature(m->temp_c, m->settings.unit_f),
                       temperature_unit(m), m->temp_from_rtc ? "(RTC)" : "");
        draw_text_clipped(0, 56, buf, 1, MODE_FOOTER_WIDTH);
    }

    draw_mode_dots(m);
}

/* ===================== PHASE 7: STOPWATCH / COUNTDOWN ===================== */

/* Căn giữa chuỗi theo chiều ngang. */
static void draw_text_centered(int y, const char *s, int scale)
{
    int x = (SSD1306_WIDTH - ssd1306_text_width(s, scale)) / 2;
    ssd1306_draw_text_scaled(x < 0 ? 0 : x, y, s, scale);
}

static void draw_text_right(int y, const char *s)
{
    ssd1306_draw_text_scaled(SSD1306_WIDTH - ssd1306_text_width(s, 1), y, s, 1);
}

static void draw_settings_screen(const display_msg_t *m)
{
    char value[16];
    static const char *const labels[4] = { "BRIGHT", "AUTO-DIM", "UNIT", "BEEP" };
    ssd1306_clear();
    ssd1306_draw_text_scaled(0, 0, "SETTINGS", 1);

    for (uint8_t row = 0; row < 4U; row++) {
        int y = 14 + row * 10;
        switch (row) {
        case 0:
            (void)snprintf(value, sizeof(value), "%u/5", (unsigned)m->settings.brightness + 1U);
            break;
        case 1:
            if (m->settings.dim_preset == 0U) {
                (void)snprintf(value, sizeof(value), "OFF");
            } else {
                static const char *const dim_labels[AUTO_DIM_PRESET_COUNT] = {
                    "OFF", "22-06", "23-07", "21-06"
                };
                (void)snprintf(value, sizeof(value), "%s", dim_labels[m->settings.dim_preset]);
            }
            break;
        case 2:
            (void)snprintf(value, sizeof(value), "%s", m->settings.unit_f ? "F" : "C");
            break;
        default:
            (void)snprintf(value, sizeof(value), "%s", m->settings.beep_on ? "ON" : "OFF");
            break;
        }
        ssd1306_draw_text_scaled(0, y, row == m->settings.cursor ? ">" : " ", 1);
        ssd1306_draw_text_scaled(10, y, labels[row], 1);
        draw_text_right(y, value);
        if (row == m->settings.cursor) {
            ssd1306_invert_region(0, y, SSD1306_WIDTH, 8);
        }
    }

    draw_text_clipped(0, 56, "UP/DN:sel OK:set", 1, MODE_FOOTER_WIDTH);
    draw_mode_dots(m);
}

static void draw_alarm_screen(const display_msg_t *m, bool highlight)
{
    char buf[8];
    ssd1306_clear();
    ssd1306_draw_text_scaled(0, 0, "ALARM", 1);

    if (!m->editing) {
        (void)snprintf(buf, sizeof(buf), "%02d:%02d", (int)m->time.hour, (int)m->time.minute);
        draw_text_right(0, buf);
    }
    if (!m->rtc_ok) {
        ssd1306_draw_text_scaled(42, 0, "RTC ERR", 1);
    }

    uint8_t alarm_hour = m->editing ? m->time.hour : m->alarm_hour;
    uint8_t alarm_minute = m->editing ? m->time.minute : m->alarm_minute;
    (void)snprintf(buf, sizeof(buf), "%02d:%02d", (int)alarm_hour, (int)alarm_minute);
    int scale = (ssd1306_text_width(buf, 4) <= SSD1306_WIDTH) ? 4 : 3;
    int time_x = (SSD1306_WIDTH - ssd1306_text_width(buf, scale)) / 2;
    ssd1306_draw_text_scaled(time_x, 8, buf, scale);

    const char *state = m->alarm_enabled ? "ON" : "OFF";
    int state_width = ssd1306_text_width(state, 2);
    int group_width = state_width + (m->alarm_enabled ? 12 : 0);
    int state_x = (SSD1306_WIDTH - group_width) / 2;
    ssd1306_draw_text_scaled(state_x, 40, state, 2);
    if (m->alarm_enabled) {
        ssd1306_draw_bitmap(state_x + state_width + 4, 44, 8, 8, icon_bell);
    }

    if (m->editing) {
        draw_text_clipped(0, 56, "OK:save HOLD:cncl", 1, MODE_FOOTER_WIDTH);
        if (highlight && m->edit_field < 2U) {
            const rect_t *r = &EDIT_RECT_ALARM[m->edit_field];
            ssd1306_invert_region(r->x, r->y, r->w, r->h);
        }
    } else {
        draw_text_clipped(0, 56, "OK:on/off H:edit", 1, MODE_FOOTER_WIDTH);
    }

    draw_mode_dots(m);
}

/* MM:SS.d (tối đa 99:59.9). Từ 100 phút trở lên đổi sang HH:MM:SS (quay vòng sau 99 giờ). */
static int format_stopwatch(uint32_t ms, char *buf, size_t n)
{
    if (ms < 6000000U) {
        (void)snprintf(buf, n, "%02u:%02u.%u", (unsigned)(ms / 60000U), (unsigned)((ms / 1000U) % 60U),
                       (unsigned)((ms / 100U) % 10U));
        return 3;                                   /* scale lớn */
    }
    (void)snprintf(buf, n, "%02u:%02u:%02u", (unsigned)((ms / 3600000U) % 100U),
                   (unsigned)((ms / 60000U) % 60U), (unsigned)((ms / 1000U) % 60U));
    return 2;
}

static void draw_stopwatch_screen(const display_msg_t *m)
{
    char buf[24];
    ssd1306_clear();
    ssd1306_draw_text_scaled(0, 0, "STOPWATCH", 1);
    draw_text_right(0, m->sw.running ? "RUN" : "STOP");

    int scale = format_stopwatch(m->sw.elapsed_ms, buf, sizeof(buf));
    draw_text_centered(scale == 3 ? 12 : 16, buf, scale);

    if (m->sw.lap_total == 0U) {
        draw_text_clipped(0, 56, "OK:go/stop DN:lap", 1, MODE_FOOTER_WIDTH);
    } else {
        int first_lap_width = 0;
        for (uint8_t row = 0; row < m->sw.row_count; row++) {
            char elapsed[16];
            (void)format_stopwatch(m->sw.row_ms[row], elapsed, sizeof(elapsed));
            (void)snprintf(buf, sizeof(buf), "L%u %s", (unsigned)m->sw.row_no[row], elapsed);
            if (ssd1306_text_width(buf, 1) <= MODE_FOOTER_WIDTH) {
                if (row == 0U) {
                    first_lap_width = ssd1306_text_width(buf, 1);
                }
                ssd1306_draw_text_scaled(0, 38 + row * 8, buf, 1);
            } else {
                (void)snprintf(buf, sizeof(buf), "L%u", (unsigned)m->sw.row_no[row]);
                if (row == 0U) {
                    first_lap_width = ssd1306_text_width(buf, 1);
                }
                ssd1306_draw_text_scaled(0, 38 + row * 8, buf, 1);
            }
        }
        if (m->sw.lap_total > 3U && !m->sw.running) {
            const char *hint = "UP:more";
            if (first_lap_width + 4 + ssd1306_text_width(hint, 1) > SSD1306_WIDTH) {
                hint = "UP";
            }
            if (first_lap_width + 4 + ssd1306_text_width(hint, 1) > SSD1306_WIDTH) {
                hint = "^";
            }
            if (first_lap_width + 4 + ssd1306_text_width(hint, 1) <= SSD1306_WIDTH) {
                draw_text_right(38, hint);
            }
        }
    }
    draw_mode_dots(m);
}

static const rect_t CD_EDIT_RECT[2] = {
    {  4, 13, 50, 30 },     /* phút */
    { 76, 13, 50, 30 },     /* giây */
};

static void draw_countdown_screen(const display_msg_t *m, bool highlight)
{
    char buf[24];
    ssd1306_clear();

    if (m->editing) {
        ssd1306_draw_text_scaled(0, 0, "SET TIMER", 1);
    } else {
        ssd1306_draw_text_scaled(0, 0, "COUNTDOWN", 1);
        const char *tag = (m->cd.state == CD_RUNNING) ? "RUN" : ((m->cd.state == CD_PAUSED) ? "PAUSE" : "IDLE");
        draw_text_right(0, tag);
    }

    /* đang chạy: làm tròn lên để không hiện 00:00 trước khi thật sự hết giờ */
    uint32_t sec = m->editing ? (m->cd.remain_ms / 1000U) : ((m->cd.remain_ms + 999U) / 1000U);
    (void)snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(sec / 60U), (unsigned)(sec % 60U));
    draw_text_centered(14, buf, 4);

    if (m->editing) {
        draw_text_clipped(0, 56, "OK:save HOLD:quit", 1, MODE_FOOTER_WIDTH);
        if (highlight && m->edit_field < 2U) {
            const rect_t *r = &CD_EDIT_RECT[m->edit_field];
            ssd1306_invert_region(r->x, r->y, r->w, r->h);
        }
    } else {
        /* thanh tiến độ: phần còn lại / tổng */
        const int bar_x = 2, bar_y = 46, bar_w = 124, bar_h = 7;
        ssd1306_draw_rect(bar_x, bar_y, bar_w, bar_h, true);
        if (m->cd.total_ms > 0U) {
            uint32_t rem = (m->cd.remain_ms > m->cd.total_ms) ? m->cd.total_ms : m->cd.remain_ms;
            int fill = (int)(((uint64_t)rem * (uint64_t)(bar_w - 4)) / m->cd.total_ms);
            ssd1306_fill_rect(bar_x + 2, bar_y + 2, fill, bar_h - 4, true);
        }
        const char *hint = (m->cd.state == CD_IDLE) ? "UP/DN:min OK:go"
                         : (m->cd.state == CD_RUNNING) ? "OK:pause" : "OK:go DN:reset";
        draw_text_clipped(0, 56, hint, 1, MODE_FOOTER_WIDTH);
    }
    draw_mode_dots(m);
}

/* Overlay toàn màn hình khi đếm ngược về 0 (hiện ở mọi chế độ), nhấp nháy đảo màu. */
static void draw_timer_done_screen(bool highlight)
{
    ssd1306_clear();
    draw_text_centered(14, "TIME UP!", 2);
    draw_text_centered(40, "Press any key", 1);
    if (highlight) {
        ssd1306_invert_region(0, 0, SSD1306_WIDTH, SSD1306_HEIGHT);
    }
}

static void draw_screen(const display_msg_t *m, bool highlight)
{
    if (m->timer_done) {
        draw_timer_done_screen(highlight);
    } else if (m->alarm_ringing) {
        draw_clock_screen(m, highlight);            /* overlay báo thức, hiện ở mọi chế độ */
    } else if (m->mode == UI_ALARM) {
        draw_alarm_screen(m, highlight);
    } else if (m->mode == UI_STOPWATCH) {
        draw_stopwatch_screen(m);
    } else if (m->mode == UI_COUNTDOWN) {
        draw_countdown_screen(m, highlight);
    } else if (m->mode == UI_SETTINGS) {
        draw_settings_screen(m);
    } else {
        draw_clock_screen(m, highlight);            /* CLOCK, ALARM */
    }
}

static void display_task(void *arg)
{
    (void)arg;
    display_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    bool have_msg = false;
    bool last_blink = false;
    bool contrast_applied = false;
    uint8_t applied_contrast = 0;
    uint32_t flush_fail = 0;
    uint32_t contrast_lock_fail = 0;
    const TickType_t blink_ticks = pdMS_TO_TICKS(BLINK_PERIOD_MS);

    while (1) {
        bool redraw = false;
        if (xQueueReceive(displayQueue, &msg, pdMS_TO_TICKS(DISPLAY_TIMEOUT_MS)) == pdTRUE) {
            have_msg = true;
            redraw = true;
        }
        if (!have_msg) {
            continue;
        }

        bool blink = ((xTaskGetTickCount() / blink_ticks) & 1U) == 0U;
        bool needs_blink = msg.editing || msg.alarm_ringing || msg.timer_done;
        if (needs_blink && blink != last_blink) {
            redraw = true;
        }
        last_blink = blink;
        if (!redraw) {
            continue;
        }

        if (!contrast_applied || msg.oled_contrast != applied_contrast) {
            if (i2c_bus_lock(I2C_MUTEX_TIMEOUT_MS)) {
                ssd1306_set_contrast(msg.oled_contrast);
                i2c_bus_unlock();
                applied_contrast = msg.oled_contrast;
                contrast_applied = true;
            } else {
                contrast_lock_fail++;
                if (contrast_lock_fail == 1U || (contrast_lock_fail % 20U) == 0U) {
                    ESP_LOGW(TAG, "cannot lock I2C bus for OLED contrast (count %lu)",
                             (unsigned long)contrast_lock_fail);
                }
            }
        }

        draw_screen(&msg, blink);

        esp_err_t err = ESP_ERR_TIMEOUT;
        if (i2c_bus_lock(I2C_MUTEX_TIMEOUT_MS)) {
            err = ssd1306_flush();
            i2c_bus_unlock();
        }
        if (err != ESP_OK) {
            flush_fail++;
            if (flush_fail == 1U || (flush_fail % 20U) == 0U) {
                ESP_LOGW(TAG, "OLED flush failed: %s (count %lu)", esp_err_to_name(err),
                         (unsigned long)flush_fail);
            }
        }
    }
}

esp_err_t task_display_start(void)
{
#ifdef OLED_DRIVER_SH1106
    const bool sh1106 = true;
#else
    const bool sh1106 = false;
#endif

    if (!i2c_bus_lock(I2C_MUTEX_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "cannot lock I2C bus for OLED init - running WITHOUT display");
        return ESP_OK;
    }
    esp_err_t err = ssd1306_init(OLED_I2C_ADDR, sh1106);
    if (err == ESP_OK) {
        ssd1306_clear();
        ssd1306_draw_text_scaled(20, 16, "CLOCK", 3);
        ssd1306_draw_text_scaled(31, 44, "Starting...", 1);
        err = ssd1306_flush();
    }
    i2c_bus_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED init failed: %s - running WITHOUT display (check wiring/address 0x%02X)",
                 esp_err_to_name(err), (unsigned)OLED_I2C_ADDR);
        return ESP_OK;
    }

    if (xTaskCreate(display_task, "DisplayTask", STACK_DISPLAY, NULL, PRIO_DISPLAY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create DisplayTask");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "DisplayTask started");
    return ESP_OK;
}
