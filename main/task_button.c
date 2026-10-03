/*
 * task_button.c - quét nút, chống rung, phân biệt nhấn ngắn / nhấn giữ / tự lặp.
 */
#include "task_button.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_types.h"
#include "app_queues.h"

static const char *TAG = "BTN";

#define BTN_DEBOUNCE_SAMPLES 3
#define BTN_CENTER_LONG_MS 800
#define BTN_ARROW_HOLD_MS 500
#define BTN_REPEAT_SLOW_MS 150
#define BTN_REPEAT_FAST_MS 60
#define BTN_REPEAT_ACCEL_MS 2000

static const gpio_num_t BTN_GPIO[BTN_COUNT] = {
    BTN_UP_GPIO, BTN_DOWN_GPIO, BTN_LEFT_GPIO, BTN_RIGHT_GPIO, BTN_CENTER_GPIO
};

typedef struct {
    bool candidate;
    uint8_t same_count;
    bool pressed;
    uint32_t held_ms;
    bool fired;
    uint32_t next_repeat_ms;
} btn_state_t;

static btn_state_t s_btn[BTN_COUNT];

static void send_button_event(button_id_t id, press_type_t press)
{
    app_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = EVT_BUTTON;
    evt.data.btn.id = id;
    evt.data.btn.press = press;

    (void)app_event_send(&evt, "ButtonTask");
}

static void update_button(button_id_t id)
{
    btn_state_t *b = &s_btn[id];
    bool raw = (gpio_get_level(BTN_GPIO[id]) == 0);

    if (raw == b->candidate) {
        if (b->same_count < BTN_DEBOUNCE_SAMPLES) {
            b->same_count++;
        }
    } else {
        b->candidate = raw;
        b->same_count = 1;
    }

    if (b->same_count >= BTN_DEBOUNCE_SAMPLES && b->candidate != b->pressed) {
        b->pressed = b->candidate;
        if (b->pressed) {
            b->held_ms = 0;
            b->fired = false;
            b->next_repeat_ms = 0;
        } else if (!b->fired) {
            send_button_event(id, PRESS_SHORT);
        }
    }

    if (!b->pressed) {
        return;
    }

    b->held_ms += BUTTON_PERIOD_MS;
    if (id == BTN_CENTER) {
        if (!b->fired && b->held_ms >= BTN_CENTER_LONG_MS) {
            b->fired = true;
            send_button_event(id, PRESS_LONG);
        }
    } else if (b->held_ms >= BTN_ARROW_HOLD_MS) {
        if (!b->fired || b->held_ms >= b->next_repeat_ms) {
            b->fired = true;
            send_button_event(id, PRESS_REPEAT);
            uint32_t interval = (b->held_ms >= BTN_REPEAT_ACCEL_MS)
                                    ? BTN_REPEAT_FAST_MS : BTN_REPEAT_SLOW_MS;
            b->next_repeat_ms = b->held_ms + interval;
        }
    }
}

static void button_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BUTTON_PERIOD_MS));
        for (int i = 0; i < BTN_COUNT; i++) {
            update_button((button_id_t)i);
        }
    }
}

esp_err_t task_button_start(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        mask |= 1ULL << BTN_GPIO[i];
    }

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    memset(s_btn, 0, sizeof(s_btn));
    if (xTaskCreate(button_task, "ButtonTask", STACK_BUTTON, NULL, PRIO_BUTTON, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create ButtonTask");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ButtonTask started (poll %d ms)", BUTTON_PERIOD_MS);
    return ESP_OK;
}
