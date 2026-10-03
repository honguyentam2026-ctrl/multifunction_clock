/*
 * task_alarm.c - AlarmTask: tạo mẫu bíp cho còi (GPIO0) và LED (GPIO8, active-LOW).
 *   Ưu tiên  : PRIO_ALARM (3)
 *   Chu kỳ   : không có - chặn trên alarmQueue
 *   Queue    : NHẬN alarmQueue (từ ModeManagerTask)
 *
 * Bộ hẹn giờ của mẫu bíp CHÍNH LÀ timeout của xQueueReceive: không vTaskDelay, không chờ bận,
 * nên lệnh ALARM_CMD_STOP đánh thức task ngay giữa nhịp bíp.
 */
#include "task_alarm.h"

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_types.h"
#include "app_queues.h"

static const char *TAG = "ALARM";

/* Một bước của mẫu: còi bật/tắt trong ms mili-giây. */
typedef struct { bool on; uint16_t ms; } step_t;

typedef struct {
    const step_t *steps;
    uint8_t       count;
    bool          loop;        /* lặp tới khi nhận STOP */
    bool          led_follow;  /* LED nháy cùng còi (chuông); false = không đụng LED (bíp phản hồi) */
} pattern_t;

static const step_t STEPS_ALARM[] = { { true, 200 }, { false, 300 } };
static const step_t STEPS_TIMER[] = {                       /* 3 bíp nhanh, nghỉ 600 ms */
    { true, 100 }, { false, 100 }, { true, 100 }, { false, 100 }, { true, 100 }, { false, 600 }
};
static const step_t STEPS_BEEP[] = { { true, 40 } };        /* bíp phản hồi nút */
static const step_t STEPS_WARN[] = { { true, 100 }, { false, 100 }, { true, 100 } };

#define PATTERN(arr, lp, follow) { (arr), (uint8_t)(sizeof(arr) / sizeof((arr)[0])), (lp), (follow) }
static const pattern_t PAT_ALARM = PATTERN(STEPS_ALARM, true, true);
static const pattern_t PAT_TIMER = PATTERN(STEPS_TIMER, true, true);
static const pattern_t PAT_BEEP  = PATTERN(STEPS_BEEP, false, false);
static const pattern_t PAT_WARN  = PATTERN(STEPS_WARN, false, false);

typedef struct {
    const pattern_t *cur;      /* NULL = yên lặng */
    uint8_t          idx;
    TickType_t       deadline; /* tick kết thúc bước hiện tại */
    bool             led_steady;
} alarm_state_t;

static void set_buzzer(bool enabled)
{
    gpio_set_level(BUZZER_GPIO, enabled ? 1 : 0);
}

static void set_led(bool enabled)
{
    /* LED_ACTIVE_LEVEL = mức logic làm LED sáng (0 với mạch active-LOW) */
    gpio_set_level(LED_GPIO, (enabled == (LED_ACTIVE_LEVEL != 0)) ? 1U : 0U);
}

static void apply_step(alarm_state_t *s)
{
    const step_t *st = &s->cur->steps[s->idx];
    set_buzzer(st->on);
    if (s->cur->led_follow) {
        set_led(st->on);
    }
    TickType_t ticks = pdMS_TO_TICKS(st->ms);
    s->deadline = xTaskGetTickCount() + ((ticks > 0U) ? ticks : 1U);
}

static void start_pattern(alarm_state_t *s, const pattern_t *p)
{
    s->cur = p;
    s->idx = 0;
    apply_step(s);
}

/* Kết thúc mẫu: còi LUÔN tắt, LED về trạng thái đứng yên (không bao giờ kẹt còi). */
static void end_pattern(alarm_state_t *s)
{
    s->cur = NULL;
    set_buzzer(false);
    set_led(s->led_steady);
}

static void advance_step(alarm_state_t *s)
{
    s->idx++;
    if (s->idx >= s->cur->count) {
        if (s->cur->loop) {
            s->idx = 0;
        } else {
            end_pattern(s);
            return;
        }
    }
    apply_step(s);
}

static void handle_cmd(alarm_state_t *s, alarm_cmd_t cmd)
{
    switch (cmd) {
    case ALARM_CMD_STOP:
        end_pattern(s);
        break;
    case ALARM_CMD_BEEP_SHORT:
    case ALARM_CMD_TEMP_WARN:
        if (s->cur != NULL && s->cur->loop) {
            break;                              /* đang đổ chuông: không cho bíp ngắn chen vào */
        }
        start_pattern(s, (cmd == ALARM_CMD_BEEP_SHORT) ? &PAT_BEEP : &PAT_WARN);
        break;
    case ALARM_CMD_RING_ALARM:
        start_pattern(s, &PAT_ALARM);
        break;
    case ALARM_CMD_RING_TIMER:
        start_pattern(s, &PAT_TIMER);
        break;
    case ALARM_CMD_LED_STEADY_ON:
    case ALARM_CMD_LED_STEADY_OFF:
        s->led_steady = (cmd == ALARM_CMD_LED_STEADY_ON);
        if (s->cur == NULL || !s->cur->led_follow) {
            set_led(s->led_steady);
        }
        break;
    default:
        break;
    }
}

static void alarm_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUZZER_GPIO) | (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    alarm_state_t st = { .cur = NULL, .idx = 0, .deadline = 0, .led_steady = false };
    set_buzzer(false);
    set_led(false);

    while (1) {
        TickType_t wait = portMAX_DELAY;
        if (st.cur != NULL) {
            int32_t remain = (int32_t)(st.deadline - xTaskGetTickCount());
            wait = (remain > 0) ? (TickType_t)remain : 0U;
        }

        alarm_cmd_t cmd;
        if (xQueueReceive(alarmQueue, &cmd, wait) == pdTRUE) {
            handle_cmd(&st, cmd);
        } else if (st.cur != NULL) {
            advance_step(&st);                  /* hết timeout = hết bước hiện tại */
        }
    }
}

esp_err_t task_alarm_start(void)
{
    if (xTaskCreate(alarm_task, "AlarmTask", STACK_ALARM, NULL, PRIO_ALARM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create AlarmTask");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AlarmTask started (buzzer GPIO%d, LED GPIO%d active-%s)",
             BUZZER_GPIO, LED_GPIO, (LED_ACTIVE_LEVEL == 0) ? "LOW" : "HIGH");
    return ESP_OK;
}
