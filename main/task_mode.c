/*
 * task_mode.c - ModeManagerTask (bộ não của ứng dụng)
 *   Mục đích : consumer DUY NHẤT của eventQueue; sở hữu MỌI trạng thái ứng dụng
 *              (chế độ, chỉnh sửa, báo thức, đồng hồ bấm giờ, đếm ngược) và gửi snapshot cho DisplayTask.
 *   Ưu tiên  : PRIO_MODE_MANAGER (3)
 *   Chu kỳ   : không có - chặn trên eventQueue (portMAX_DELAY)
 *   Queue    : NHẬN eventQueue
 *              GỬI  displayQueue (xQueueOverwrite), alarmQueue, rtcCmdQueue
 *
 * Phase 7: thêm STOPWATCH + COUNTDOWN. Cả hai chạy NỀN khi người dùng đổi chế độ,
 * thời gian tính từ esp_timer_get_time() (không đếm số tick) nên không bị trôi khi rớt tick.
 */
#include "task_mode.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "app_types.h"
#include "app_queues.h"
#include "ds3231.h"
#include "storage.h"

static const char *TAG = "MODE";

static const uint8_t s_brightness_contrast[OLED_BRIGHTNESS_LEVEL_COUNT] =
    OLED_BRIGHTNESS_CONTRAST_VALUES;
static const uint8_t s_dim_start_hour[AUTO_DIM_PRESET_COUNT] = AUTO_DIM_START_HOURS;
static const uint8_t s_dim_end_hour[AUTO_DIM_PRESET_COUNT] = AUTO_DIM_END_HOURS;

/* Trường chỉnh sửa của giờ đồng hồ; báo thức dùng 0..1 (giờ, phút); đếm ngược dùng 0..1 (phút, giây). */
enum { FIELD_HOUR, FIELD_MINUTE, FIELD_DAY, FIELD_MONTH, FIELD_YEAR, FIELD_COUNT };
enum { CD_FIELD_MIN, CD_FIELD_SEC };

#define ALARM_AUTOSTOP_US  (60LL * 1000000LL)       /* chuông báo thức tự tắt sau 60 s */
#define ALARM_SNOOZE_US    (5LL * 60LL * 1000000LL) /* báo lại sau 5 phút */
#define TIMER_AUTOSTOP_US  (30LL * 1000000LL)       /* "TIME UP" tự tắt sau 30 s */
#define CD_DEFAULT_S       300U                     /* mặc định 05:00 */
#define CD_STEP_MS         60000U                   /* UP/DOWN = +-1 phút */
#define CD_MAX_MS          (99U * 60000U + 59000U)  /* tối đa 99:59 */

/* ĐANG chỉnh cái gì. Chốt MỘT lần lúc vào chế độ chỉnh, mọi chỗ khác chỉ đọc biến này
 * (KHÔNG suy ra từ msg.mode) để bấm OK không bao giờ ghi nhầm vào DS3231. */
typedef enum { EDIT_TIME, EDIT_ALARM, EDIT_COUNTDOWN } edit_target_t;

typedef struct {
    display_msg_t msg;              /* snapshot gửi cho DisplayTask */
    rtc_time_t    cur_time;         /* giờ thật gần nhất từ ClockTask */
    edit_target_t edit_target;
    bool          ready;            /* đã có dữ liệu để vẽ */
    bool          clock_seen;
    uint32_t      last_trigger_key; /* chống kêu lặp trong cùng một phút */
    int64_t       last_button_us;

    /* báo thức: mốc thời gian bằng esp_timer */
    int64_t       alarm_start_us;
    int64_t       snooze_deadline_us;

    /* đồng hồ bấm giờ */
    bool          sw_running;
    int64_t       sw_accum_us;      /* thời gian đã cộng dồn khi dừng */
    int64_t       sw_start_us;      /* mốc lúc bấm start */
    uint32_t      sw_laps_ms[SW_MAX_LAPS];
    uint16_t      sw_lap_total;
    uint16_t      sw_scroll;        /* số vòng lùi so với trang mới nhất */

    /* đếm ngược */
    countdown_state_t cd_state;
    uint32_t      cd_total_ms;      /* thời lượng đã cài */
    uint32_t      cd_edit_ms;       /* giá trị đang chỉnh */
    int64_t       cd_end_us;        /* mốc kết thúc khi RUNNING */
    uint32_t      cd_paused_ms;     /* thời gian còn lại khi PAUSED */
    uint32_t      cd_shown_sec;     /* giây đã gửi gần nhất (giảm số lần vẽ lại) */
    bool          cd_total_dirty;   /* cài đặt thay đổi bằng UP/DOWN, chưa ghi NVS */
    int64_t       timer_done_us;
} mode_ctx_t;

/* ============================ tiện ích ============================ */

static int wrap(int v, int lo, int hi)
{
    if (v > hi) {
        return lo;
    }
    if (v < lo) {
        return hi;
    }
    return v;
}

static bool is_step(press_type_t p)
{
    return p == PRESS_SHORT || p == PRESS_REPEAT;
}

static void edit_adjust(rtc_time_t *t, uint8_t field, int delta)
{
    switch (field) {
    case FIELD_HOUR:
        t->hour = (uint8_t)wrap((int)t->hour + delta, 0, 23);
        break;
    case FIELD_MINUTE:
        t->minute = (uint8_t)wrap((int)t->minute + delta, 0, 59);
        break;
    case FIELD_DAY:
        t->day = (uint8_t)wrap((int)t->day + delta, 1, (int)ds3231_days_in_month(t->year, t->month));
        break;
    case FIELD_MONTH:
        t->month = (uint8_t)wrap((int)t->month + delta, 1, 12);
        break;
    case FIELD_YEAR:
        t->year = (uint16_t)wrap((int)t->year + delta, 2000, 2099);
        break;
    default:
        break;
    }
    uint8_t dim = ds3231_days_in_month(t->year, t->month);
    if (t->day > dim) {
        t->day = dim;
    }
    t->weekday = ds3231_calc_weekday(t->year, t->month, t->day);
}

static void edit_adjust_alarm(rtc_time_t *t, uint8_t field, int delta)
{
    if (field == FIELD_HOUR) {
        t->hour = (uint8_t)wrap((int)t->hour + delta, 0, 23);
    } else if (field == FIELD_MINUTE) {
        t->minute = (uint8_t)wrap((int)t->minute + delta, 0, 59);
    }
}

/* Chỉnh thời lượng đếm ngược: trường 0 = phút (0..99), trường 1 = giây (0..59). */
static uint32_t edit_adjust_countdown(uint32_t ms, uint8_t field, int delta)
{
    int minutes = (int)(ms / 60000U);
    int seconds = (int)((ms / 1000U) % 60U);
    if (field == CD_FIELD_MIN) {
        minutes = wrap(minutes + delta, 0, 99);
    } else {
        seconds = wrap(seconds + delta, 0, 59);
    }
    return (uint32_t)minutes * 60000U + (uint32_t)seconds * 1000U;
}

static int field_count(edit_target_t target)
{
    return (target == EDIT_TIME) ? FIELD_COUNT : 2;
}

static const char *edit_name(edit_target_t target)
{
    switch (target) {
    case EDIT_ALARM:
        return "alarm";
    case EDIT_COUNTDOWN:
        return "countdown";
    default:
        return "time";
    }
}

static void log_time(const char *what, const rtc_time_t *t)
{
    ESP_LOGI(TAG, "%s %04d-%02d-%02d %02d:%02d:%02d", what,
             (int)t->year, (int)t->month, (int)t->day,
             (int)t->hour, (int)t->minute, (int)t->second);
}

/* ============================ âm thanh ============================ */

static void send_alarm_cmd(alarm_cmd_t cmd)
{
    if (xQueueSend(alarmQueue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "alarmQueue full, cmd %d dropped", (int)cmd);
    }
}

static void send_led_steady(const mode_ctx_t *c)
{
    send_alarm_cmd(c->msg.alarm_enabled ? ALARM_CMD_LED_STEADY_ON : ALARM_CMD_LED_STEADY_OFF);
}

/* Chọn âm thanh theo trạng thái hiện tại. Có hai nguồn kêu (báo thức, hết giờ đếm ngược)
 * nhưng chỉ một còi: "TIME UP" được ưu tiên vì overlay của nó nằm trên cùng.
 * Gọi hàm này sau MỌI lần đổi alarm_ringing / timer_done để còi luôn khớp với màn hình. */
static void sync_sound(const mode_ctx_t *c)
{
    if (c->msg.timer_done) {
        send_alarm_cmd(ALARM_CMD_RING_TIMER);
    } else if (c->msg.alarm_ringing) {
        send_alarm_cmd(ALARM_CMD_RING_ALARM);
    } else {
        send_alarm_cmd(ALARM_CMD_STOP);     /* AlarmTask trả LED về trạng thái đứng yên */
        send_led_steady(c);
    }
}

/* Hủy chỉnh sửa (không lưu). Dùng khi chuông/hết giờ chen vào lúc đang chỉnh. */
static void cancel_edit(mode_ctx_t *c)
{
    display_msg_t *m = &c->msg;
    if (!m->editing) {
        return;
    }
    ESP_LOGI(TAG, "edit %s: cancelled", edit_name(c->edit_target));
    m->editing = false;
    m->time = c->cur_time;
}

/* ============================ báo thức ============================ */

static void save_alarm_now(mode_ctx_t *c)
{
    display_msg_t *m = &c->msg;
    esp_err_t err = storage_save_alarm(m->alarm_hour, m->alarm_minute, m->alarm_enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage save alarm failed: %s", esp_err_to_name(err));
    }
}

static void alarm_start_ring(mode_ctx_t *c, int64_t now_us)
{
    cancel_edit(c);
    c->msg.alarm_ringing = true;
    c->msg.snooze_active = false;
    c->alarm_start_us = now_us;
    sync_sound(c);
    ESP_LOGI(TAG, "ALARM ringing");
}

static void alarm_dismiss(mode_ctx_t *c, bool snooze)
{
    c->msg.alarm_ringing = false;
    c->msg.snooze_active = snooze;
    if (snooze) {
        c->snooze_deadline_us = esp_timer_get_time() + ALARM_SNOOZE_US;
        ESP_LOGI(TAG, "alarm snoozed 5 min");
    }
    sync_sound(c);
}

static void maybe_trigger_alarm(mode_ctx_t *c, const rtc_time_t *t)
{
    display_msg_t *m = &c->msg;
    if (!m->alarm_enabled || m->alarm_ringing || t->second != 0) {
        return;
    }
    uint32_t key = ((uint32_t)t->year * 10000U + (uint32_t)t->month * 100U + (uint32_t)t->day) * 1440U +
                   ((uint32_t)t->hour * 60U + (uint32_t)t->minute);
    if (t->hour == m->alarm_hour && t->minute == m->alarm_minute && key != c->last_trigger_key) {
        c->last_trigger_key = key;
        alarm_start_ring(c, esp_timer_get_time());
    }
}

/* ============================ đồng hồ bấm giờ ============================ */

static uint32_t sw_elapsed_ms(const mode_ctx_t *c, int64_t now_us)
{
    int64_t us = c->sw_accum_us;
    if (c->sw_running) {
        us += now_us - c->sw_start_us;
    }
    return (uint32_t)(us / 1000);
}

static void handle_button_stopwatch(mode_ctx_t *c, button_id_t id, press_type_t press)
{
    int64_t now = esp_timer_get_time();

    if (id == BTN_CENTER && press == PRESS_SHORT) {
        if (c->sw_running) {
            c->sw_accum_us += now - c->sw_start_us;
            c->sw_running = false;
            ESP_LOGI(TAG, "stopwatch stop at %lu ms", (unsigned long)sw_elapsed_ms(c, now));
        } else {
            c->sw_start_us = now;
            c->sw_running = true;
            c->sw_scroll = 0;
            ESP_LOGI(TAG, "stopwatch start");
        }
        return;
    }

    if (id == BTN_DOWN && press == PRESS_SHORT) {
        if (c->sw_running) {
            uint32_t lap_ms = sw_elapsed_ms(c, now);
            c->sw_lap_total++;
            uint16_t index = (uint16_t)((c->sw_lap_total - 1U) % SW_MAX_LAPS);
            c->sw_laps_ms[index] = lap_ms;
            c->sw_scroll = 0;
            ESP_LOGI(TAG, "stopwatch lap %u = %lu ms", (unsigned)c->sw_lap_total,
                     (unsigned long)lap_ms);
        } else {
            c->sw_accum_us = 0;
            memset(c->sw_laps_ms, 0, sizeof(c->sw_laps_ms));
            c->sw_lap_total = 0;
            c->sw_scroll = 0;
            ESP_LOGI(TAG, "stopwatch reset");
        }
        return;
    }

    if (id == BTN_UP && press == PRESS_SHORT && !c->sw_running) {
        uint16_t retained = (c->sw_lap_total < SW_MAX_LAPS)
                          ? c->sw_lap_total : SW_MAX_LAPS;
        if (retained > 3U) {
            uint16_t max_scroll = (uint16_t)(((retained - 1U) / 3U) * 3U);
            c->sw_scroll = (c->sw_scroll >= max_scroll)
                         ? 0U : (uint16_t)(c->sw_scroll + 3U);
        }
    }
}

/* ============================ đếm ngược ============================ */

static bool cd_save_if_dirty(mode_ctx_t *c)
{
    if (!c->cd_total_dirty) {
        return true;
    }
    esp_err_t err = storage_save_cd_total(c->cd_total_ms / 1000U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage save countdown failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "NVS: saved countdown %lu s", (unsigned long)(c->cd_total_ms / 1000U));
    c->cd_total_dirty = false;
    return true;
}

static uint32_t cd_remaining_ms(const mode_ctx_t *c, int64_t now_us)
{
    switch (c->cd_state) {
    case CD_IDLE:
        return c->cd_total_ms;
    case CD_RUNNING: {
        int64_t rem_us = c->cd_end_us - now_us;
        return (rem_us > 0) ? (uint32_t)(rem_us / 1000) : 0U;
    }
    case CD_PAUSED:
        return c->cd_paused_ms;
    default:
        return 0U;
    }
}

static void timer_finish(mode_ctx_t *c, int64_t now_us)
{
    cancel_edit(c);
    c->cd_state = CD_DONE;
    c->msg.timer_done = true;
    c->timer_done_us = now_us;
    sync_sound(c);
    ESP_LOGI(TAG, "COUNTDOWN done - TIME UP");
}

static void timer_dismiss(mode_ctx_t *c)
{
    c->msg.timer_done = false;
    c->cd_state = CD_IDLE;
    sync_sound(c);
    ESP_LOGI(TAG, "TIME UP dismissed, countdown back to IDLE");
}

static void handle_button_countdown(mode_ctx_t *c, button_id_t id, press_type_t press)
{
    display_msg_t *m = &c->msg;
    int64_t now = esp_timer_get_time();

    switch (id) {
    case BTN_UP:
    case BTN_DOWN:
        if (!is_step(press)) {
            return;
        }
        if (c->cd_state == CD_IDLE) {
            uint32_t total = c->cd_total_ms;
            if (id == BTN_UP) {
                total = (total + CD_STEP_MS > CD_MAX_MS) ? CD_MAX_MS : total + CD_STEP_MS;
            } else if (total > CD_STEP_MS) {
                total -= CD_STEP_MS;
            }
            if (total != c->cd_total_ms) {
                c->cd_total_ms = total;
                c->cd_total_dirty = true;     /* ghi NVS khi bấm start, không ghi mỗi lần lặp */
            }
        } else if (id == BTN_DOWN && press == PRESS_SHORT && c->cd_state == CD_PAUSED) {
            c->cd_state = CD_IDLE;
            ESP_LOGI(TAG, "countdown reset");
        }
        return;

    case BTN_CENTER:
        if (press == PRESS_SHORT) {
            if (c->cd_state == CD_IDLE) {
                (void)cd_save_if_dirty(c);
                c->cd_end_us = now + (int64_t)c->cd_total_ms * 1000;
                c->cd_shown_sec = 0;
                c->cd_state = CD_RUNNING;
                ESP_LOGI(TAG, "countdown start %lu s", (unsigned long)(c->cd_total_ms / 1000U));
            } else if (c->cd_state == CD_RUNNING) {
                c->cd_paused_ms = cd_remaining_ms(c, now);
                c->cd_state = CD_PAUSED;
                ESP_LOGI(TAG, "countdown pause, %lu ms left", (unsigned long)c->cd_paused_ms);
            } else if (c->cd_state == CD_PAUSED) {
                c->cd_end_us = now + (int64_t)c->cd_paused_ms * 1000;
                c->cd_shown_sec = 0;
                c->cd_state = CD_RUNNING;
                ESP_LOGI(TAG, "countdown resume");
            }
        } else if (press == PRESS_LONG) {
            if (c->cd_state != CD_IDLE) {
                ESP_LOGI(TAG, "countdown: reset to IDLE before editing");
                return;
            }
            c->edit_target = EDIT_COUNTDOWN;
            c->cd_edit_ms = c->cd_total_ms;
            m->editing = true;
            m->edit_field = CD_FIELD_MIN;
            ESP_LOGI(TAG, "edit countdown: start");
        }
        return;

    default:
        return;
    }
}

/* ============================ chỉnh sửa (dùng chung) ============================ */

/* Xử lý nút khi đang ở chế độ chỉnh (giờ đồng hồ / báo thức / đếm ngược). */
static void handle_button_editing(mode_ctx_t *c, button_id_t id, press_type_t press)
{
    display_msg_t *m = &c->msg;

    switch (id) {
    case BTN_UP:
    case BTN_DOWN:
        if (is_step(press)) {
            int delta = (id == BTN_UP) ? 1 : -1;
            switch (c->edit_target) {
            case EDIT_TIME:
                edit_adjust(&m->time, m->edit_field, delta);
                break;
            case EDIT_ALARM:
                edit_adjust_alarm(&m->time, m->edit_field, delta);
                break;
            default:
                c->cd_edit_ms = edit_adjust_countdown(c->cd_edit_ms, m->edit_field, delta);
                break;
            }
        }
        return;

    case BTN_LEFT:
    case BTN_RIGHT:
        if (is_step(press)) {
            m->edit_field = (uint8_t)wrap((int)m->edit_field + ((id == BTN_RIGHT) ? 1 : -1),
                                          0, field_count(c->edit_target) - 1);
        }
        return;

    case BTN_CENTER:
        if (press == PRESS_SHORT) {
            switch (c->edit_target) {
            case EDIT_ALARM:
                m->alarm_hour = m->time.hour;
                m->alarm_minute = m->time.minute;
                m->editing = false;
                ESP_LOGI(TAG, "edit alarm: saved %02d:%02d", (int)m->alarm_hour, (int)m->alarm_minute);
                save_alarm_now(c);
                m->time = c->cur_time;
                return;

            case EDIT_COUNTDOWN:
                if (c->cd_edit_ms == 0U) {
                    ESP_LOGW(TAG, "edit countdown: duration must be > 0, not saved");
                    return;                 /* ở lại chế độ chỉnh */
                }
                uint32_t previous_total_ms = c->cd_total_ms;
                bool previous_dirty = c->cd_total_dirty;
                c->cd_total_ms = c->cd_edit_ms;
                c->cd_total_dirty = true;
                if (!cd_save_if_dirty(c)) {
                    c->cd_total_ms = previous_total_ms;
                    c->cd_total_dirty = previous_dirty;
                    return;
                }
                m->editing = false;
                ESP_LOGI(TAG, "edit countdown: saved %lu s", (unsigned long)(c->cd_total_ms / 1000U));
                return;

            default: {
                rtc_cmd_t cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.time = m->time;
                cmd.time.second = 0;
                if (xQueueSend(rtcCmdQueue, &cmd, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "rtcCmdQueue full, time not saved");
                } else {
                    log_time("edit time: saved", &cmd.time);
                    c->cur_time = cmd.time;
                }
                m->editing = false;
                m->time = c->cur_time;
                return;
            }
            }
        }
        if (press == PRESS_LONG) {
            cancel_edit(c);
        }
        return;

    default:
        return;
    }
}

/* ============================ nút: CLOCK / ALARM / chuyển chế độ ============================ */

static void handle_mode_switch(mode_ctx_t *c, bool next)
{
    ui_mode_t mode = c->msg.mode;
    mode = (ui_mode_t)((next ? (mode + 1) : (mode + UI_MODE_COUNT - 1)) % UI_MODE_COUNT);
    c->msg.mode = mode;
    c->msg.editing = false;
    c->cd_shown_sec = 0;
}

static bool update_oled_contrast(mode_ctx_t *c, int64_t now_us)
{
    display_msg_t *m = &c->msg;
    uint8_t contrast = s_brightness_contrast[m->settings.brightness];
    uint8_t preset = m->settings.dim_preset;
    if (preset > 0U && preset < AUTO_DIM_PRESET_COUNT && c->clock_seen &&
        now_us - c->last_button_us >= 10000000LL &&
        m->mode != UI_SETTINGS && !m->alarm_ringing && !m->timer_done) {
        uint8_t start = s_dim_start_hour[preset];
        uint8_t end = s_dim_end_hour[preset];
        uint8_t hour = c->cur_time.hour;
        bool in_dim_hours = (start < end)
                          ? (hour >= start && hour < end)
                          : (hour >= start || hour < end);
        if (in_dim_hours) {
            contrast = DIM_CONTRAST;
        }
    }
    if (m->oled_contrast == contrast) {
        return false;
    }
    m->oled_contrast = contrast;
    return true;
}

static void handle_button_settings(mode_ctx_t *c, button_id_t id, press_type_t press)
{
    display_msg_t *m = &c->msg;
    if ((id == BTN_UP || id == BTN_DOWN) && is_step(press)) {
        int delta = (id == BTN_UP) ? -1 : 1;
        m->settings.cursor = (uint8_t)wrap((int)m->settings.cursor + delta, 0, 3);
        return;
    }
    if (id != BTN_CENTER || press != PRESS_SHORT) {
        return;
    }

    switch (m->settings.cursor) {
    case 0:
        m->settings.brightness = (uint8_t)((m->settings.brightness + 1U) %
                                            OLED_BRIGHTNESS_LEVEL_COUNT);
        break;
    case 1:
        m->settings.dim_preset = (uint8_t)((m->settings.dim_preset + 1U) %
                                           AUTO_DIM_PRESET_COUNT);
        break;
    case 2:
        m->settings.unit_f = !m->settings.unit_f;
        break;
    case 3:
        m->settings.beep_on = !m->settings.beep_on;
        break;
    default:
        return;
    }

    esp_err_t err = storage_save_settings(m->settings.brightness, m->settings.dim_preset,
                                          m->settings.unit_f, m->settings.beep_on);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage save settings failed: %s", esp_err_to_name(err));
    }
    (void)update_oled_contrast(c, esp_timer_get_time());
    if (m->settings.beep_on) {
        send_alarm_cmd(ALARM_CMD_BEEP_SHORT);
    }
}

static void handle_button_alarm(mode_ctx_t *c, button_id_t id, press_type_t press)
{
    display_msg_t *m = &c->msg;
    if (id != BTN_CENTER) {
        return;
    }
    if (press == PRESS_SHORT) {
        m->alarm_enabled = !m->alarm_enabled;
        if (!m->alarm_enabled) {
            m->snooze_active = false;        /* tắt báo thức thì hủy luôn lần báo lại */
        }
        save_alarm_now(c);
        send_led_steady(c);
    } else if (press == PRESS_LONG) {
        c->edit_target = EDIT_ALARM;
        m->editing = true;
        m->edit_field = FIELD_HOUR;
        m->time = c->cur_time;
        m->time.hour = m->alarm_hour;
        m->time.minute = m->alarm_minute;
        m->time.second = 0;
        ESP_LOGI(TAG, "edit alarm: start");
    }
}

static void handle_button_clock(mode_ctx_t *c, button_id_t id, press_type_t press)
{
    display_msg_t *m = &c->msg;
    if (id == BTN_CENTER && press == PRESS_LONG) {
        /* Mất DS3231 vẫn cho chỉnh: ClockTask sẽ áp dụng giờ mới vào đồng hồ phần mềm. */
        c->edit_target = EDIT_TIME;
        m->editing = true;
        m->edit_field = FIELD_HOUR;
        m->time = c->cur_time;
        m->time.second = 0;
        ESP_LOGI(TAG, "edit time: start");
    }
}

static void handle_button(mode_ctx_t *c, button_id_t id, press_type_t press)
{
    display_msg_t *m = &c->msg;
    int64_t now_us = esp_timer_get_time();
    c->last_button_us = now_us;
    (void)update_oled_contrast(c, now_us);

    /* Quy tắc toàn cục: khi "TIME UP" hoặc báo thức đang kêu, nút ĐẦU TIÊN chỉ để tắt
     * (hoặc báo lại) và bị "ăn" - không làm việc gì khác. Thứ tự khớp với overlay trên màn hình. */
    if (m->timer_done) {
        timer_dismiss(c);
        return;
    }
    if (m->alarm_ringing) {
        alarm_dismiss(c, id == BTN_UP && press == PRESS_SHORT);
        return;
    }

    if (m->mode != UI_SETTINGS && press == PRESS_SHORT && m->settings.beep_on) {
        send_alarm_cmd(ALARM_CMD_BEEP_SHORT);
    }

    if (m->editing) {
        handle_button_editing(c, id, press);
        return;
    }

    if (press == PRESS_SHORT && (id == BTN_LEFT || id == BTN_RIGHT)) {
        handle_mode_switch(c, id == BTN_RIGHT);
        (void)update_oled_contrast(c, now_us);
        return;
    }

    switch (m->mode) {
    case UI_CLOCK:
        handle_button_clock(c, id, press);
        break;
    case UI_ALARM:
        handle_button_alarm(c, id, press);
        break;
    case UI_STOPWATCH:
        handle_button_stopwatch(c, id, press);
        break;
    case UI_COUNTDOWN:
        handle_button_countdown(c, id, press);
        break;
    case UI_SETTINGS:
        handle_button_settings(c, id, press);
        break;
    default:
        break;
    }
}

/* ============================ sự kiện ============================ */

static void handle_env(mode_ctx_t *c, const app_event_t *e)
{
    display_msg_t *m = &c->msg;
    float t = e->data.env.temp_c;

    m->temp_c = t;
    m->temp_from_rtc = e->data.env.from_rtc;
    m->env_valid = true;

    if (t >= TEMP_WARN_C) {
        if (!m->temp_warn) {
            m->temp_warn = true;
            ESP_LOGW(TAG, "temperature warning: %.1f C >= %.1f C", (double)t, (double)TEMP_WARN_C);
            send_alarm_cmd(ALARM_CMD_TEMP_WARN);
        }
    } else if (t < (TEMP_WARN_C - TEMP_WARN_HYST_C) && m->temp_warn) {
        m->temp_warn = false;
        ESP_LOGI(TAG, "temperature back to normal: %.1f C", (double)t);
    }

    m->hum_valid = e->data.env.humidity_valid;
    if (m->hum_valid) {
        m->hum_pct = e->data.env.humidity_pct;
    }
}

/* Xử lý mỗi 100 ms. Trả về true nếu có thay đổi nhìn thấy được (cần gửi snapshot mới). */
static bool on_tick(mode_ctx_t *c)
{
    display_msg_t *m = &c->msg;
    int64_t now = esp_timer_get_time();
    bool dirty = false;

    /* báo thức: hết 5 phút báo lại -> kêu tiếp */
    if (m->snooze_active && !m->alarm_ringing && now >= c->snooze_deadline_us) {
        ESP_LOGI(TAG, "snooze elapsed");
        alarm_start_ring(c, now);
        dirty = true;
    }

    if (update_oled_contrast(c, now)) {
        dirty = true;
    }

    /* báo thức: không ai bấm thì tự tắt sau 60 s (dùng esp_timer nên không lệch dù rớt tick) */
    if (m->alarm_ringing && (now - c->alarm_start_us) >= ALARM_AUTOSTOP_US) {
        ESP_LOGI(TAG, "alarm auto-stop after 60 s");
        alarm_dismiss(c, false);
        dirty = true;
    }

    /* đếm ngược: chạy nền ở mọi chế độ */
    if (c->cd_state == CD_RUNNING) {
        int64_t rem_us = c->cd_end_us - now;
        if (rem_us <= 0) {
            timer_finish(c, now);
            dirty = true;
        } else if (m->mode == UI_COUNTDOWN) {
            uint32_t sec = (uint32_t)((rem_us + 999999) / 1000000);   /* làm tròn lên: không hiện 00:00 sớm */
            if (sec != c->cd_shown_sec) {
                c->cd_shown_sec = sec;
                dirty = true;
            }
        }
    }

    /* "TIME UP" tự tắt sau 30 s */
    if (m->timer_done && (now - c->timer_done_us) >= TIMER_AUTOSTOP_US) {
        ESP_LOGI(TAG, "TIME UP auto-stop after 30 s");
        timer_dismiss(c);
        dirty = true;
    }

    /* bấm giờ: chỉ vẽ lại khi đang chạy VÀ đang hiển thị (giảm tải bus I2C chung với DS3231) */
    if (c->sw_running && m->mode == UI_STOPWATCH && !m->alarm_ringing && !m->timer_done) {
        dirty = true;
    }
    return dirty;
}

/* Trả về true nếu cần gửi snapshot mới cho DisplayTask. */
static bool handle_event(mode_ctx_t *c, const app_event_t *e)
{
    display_msg_t *m = &c->msg;

    switch (e->type) {
    case EVT_TIME:
        c->cur_time = e->data.time;
        c->clock_seen = true;
        c->ready = true;
        m->rtc_ok = !e->soft_time;
        /* Đang chỉnh giờ đồng hồ thì GIỮ giá trị đang chỉnh; đang chỉnh báo thức thì chỉ giữ giờ/phút. */
        if (!(m->editing && c->edit_target == EDIT_TIME)) {
            uint8_t edit_hour = m->time.hour;
            uint8_t edit_minute = m->time.minute;
            m->time = e->data.time;
            if (m->editing && c->edit_target == EDIT_ALARM) {
                m->time.hour = edit_hour;
                m->time.minute = edit_minute;
            }
        }
        maybe_trigger_alarm(c, &e->data.time);
        (void)update_oled_contrast(c, esp_timer_get_time());
        return true;

    case EVT_RTC_ERROR:
        m->rtc_ok = false;
        ESP_LOGW(TAG, "RTC error reported by ClockTask");
        return true;

    case EVT_ENV:
        handle_env(c, e);
        return true;

    case EVT_BUTTON:
        handle_button(c, e->data.btn.id, e->data.btn.press);
        return true;

    case EVT_TICK_100MS:
        return on_tick(c);

    default:
        return false;
    }
}

/* Cập nhật các trường tính từ đồng hồ (bấm giờ, đếm ngược) ngay trước khi gửi snapshot. */
static void refresh_snapshot(mode_ctx_t *c)
{
    display_msg_t *m = &c->msg;
    int64_t now = esp_timer_get_time();

    m->sw.running = c->sw_running;
    m->sw.elapsed_ms = sw_elapsed_ms(c, now);
    m->sw.lap_total = c->sw_lap_total;

    uint16_t retained = (c->sw_lap_total < SW_MAX_LAPS)
                      ? c->sw_lap_total : SW_MAX_LAPS;
    uint16_t scroll = c->sw_running ? 0U : c->sw_scroll;
    uint16_t end = retained - scroll;
    uint16_t first = (end > 3U) ? (uint16_t)(end - 3U) : 0U;
    m->sw.row_count = (uint8_t)(end - first);
    for (uint8_t row = 0; row < m->sw.row_count; row++) {
        uint16_t chronological_index = (uint16_t)(first + row);
        uint16_t lap_no = (uint16_t)(c->sw_lap_total - retained + 1U + chronological_index);
        uint16_t ring_index = (uint16_t)((lap_no - 1U) % SW_MAX_LAPS);
        m->sw.row_no[row] = lap_no;
        m->sw.row_ms[row] = c->sw_laps_ms[ring_index];
    }

    m->cd.state = c->cd_state;
    m->cd.total_ms = c->cd_total_ms;
    m->cd.remain_ms = cd_remaining_ms(c, now);
    if (m->editing && c->edit_target == EDIT_COUNTDOWN) {
        m->cd.total_ms = c->cd_edit_ms;      /* màn hình chỉnh hiển thị giá trị đang chỉnh */
        m->cd.remain_ms = c->cd_edit_ms;
    }
}

static void mode_task(void *arg)
{
    (void)arg;
    mode_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.msg.mode = UI_CLOCK;
    ctx.cd_total_ms = CD_DEFAULT_S * 1000U;
    ctx.last_button_us = esp_timer_get_time();
    ctx.msg.settings.brightness = OLED_BRIGHTNESS_LEVEL_COUNT - 1U;
    ctx.msg.settings.dim_preset = 0;
    ctx.msg.settings.unit_f = false;
    ctx.msg.settings.beep_on = ENABLE_BUTTON_BEEP != 0;
    ctx.msg.oled_contrast = s_brightness_contrast[ctx.msg.settings.brightness];

    uint8_t alarm_h = 7;
    uint8_t alarm_m = 0;
    bool alarm_enabled = false;
    esp_err_t err = storage_load_alarm(&alarm_h, &alarm_m, &alarm_enabled);
    if (err == ESP_OK) {
        ctx.msg.alarm_enabled = alarm_enabled;
        ctx.msg.alarm_hour = alarm_h;
        ctx.msg.alarm_minute = alarm_m;
        ESP_LOGI(TAG, "NVS: loaded alarm %02d:%02d %s", alarm_h, alarm_m, alarm_enabled ? "ON" : "OFF");
    } else {
        ESP_LOGW(TAG, "NVS load alarm failed (%s), using defaults", esp_err_to_name(err));
        ctx.msg.alarm_hour = alarm_h;
        ctx.msg.alarm_minute = alarm_m;
    }

    err = storage_load_settings(&ctx.msg.settings.brightness, &ctx.msg.settings.dim_preset,
                               &ctx.msg.settings.unit_f, &ctx.msg.settings.beep_on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS load settings failed (%s), using defaults", esp_err_to_name(err));
        ctx.msg.settings.brightness = OLED_BRIGHTNESS_LEVEL_COUNT - 1U;
        ctx.msg.settings.dim_preset = 0;
        ctx.msg.settings.unit_f = false;
        ctx.msg.settings.beep_on = ENABLE_BUTTON_BEEP != 0;
    }
    ctx.msg.oled_contrast = s_brightness_contrast[ctx.msg.settings.brightness];

    uint32_t cd_s = 0;
    err = storage_load_cd_total(&cd_s);
    if (err == ESP_OK && cd_s > 0U && cd_s <= (CD_MAX_MS / 1000U)) {
        ctx.cd_total_ms = cd_s * 1000U;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS load countdown failed (%s), using default", esp_err_to_name(err));
    } else if (cd_s > (CD_MAX_MS / 1000U)) {
        ESP_LOGW(TAG, "stored countdown %lu s is out of range, using default", (unsigned long)cd_s);
    }
    ESP_LOGI(TAG, "countdown preset %lu s", (unsigned long)(ctx.cd_total_ms / 1000U));

    send_led_steady(&ctx);                   /* LED sáng đứng yên nếu báo thức đang bật */

    app_event_t e;
    while (1) {
        if (xQueueReceive(eventQueue, &e, portMAX_DELAY) == pdTRUE) {
            bool changed = handle_event(&ctx, &e);
            if (changed && ctx.ready) {
                refresh_snapshot(&ctx);
                (void)xQueueOverwrite(displayQueue, &ctx.msg);
            }
        }
    }
}

esp_err_t task_mode_start(void)
{
    if (xTaskCreate(mode_task, "ModeManagerTask", STACK_MODE_MANAGER, NULL, PRIO_MODE_MANAGER, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create ModeManagerTask");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ModeManagerTask started");
    return ESP_OK;
}
