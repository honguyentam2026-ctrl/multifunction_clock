#include "dht.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#define DHT_START_LOW_MS        20      /* giữ LOW >= 18 ms (nằm NGOÀI vùng găng) */
#define DHT_WAIT_TIMEOUT_US     100     /* timeout mỗi vòng chờ cạnh */
#define DHT_BIT_TIMEOUT_US      150
#define DHT_BIT_THRESHOLD_US    40      /* xung HIGH > 40 us => bit 1 (26-28 us = 0, ~70 us = 1) */
#define DHT_POWERUP_WAIT_US     1500000 /* chờ ~1.5 s sau khi cấp nguồn */

static const char *TAG = "TMP";

static gpio_num_t s_pin;
static dht_type_t s_type;
static int64_t    s_last_us;
static bool       s_inited = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Chờ chân đạt mức 'level'. Trả về thời gian chờ (us) hoặc -1 nếu quá timeout (không bao giờ treo). */
static int IRAM_ATTR wait_level(int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(s_pin) != level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - start);
}

/* Đọc 40 bit. Gọi TRONG vùng găng (đã tắt ngắt). */
static esp_err_t IRAM_ATTR read_frame(uint8_t data[5])
{
    memset(data, 0, 5);

    /* Nhả đường dây (pull-up kéo lên HIGH), rồi chờ cảm biến phản hồi:
     * LOW ~80 us, HIGH ~80 us, sau đó mỗi bit = LOW ~50 us + HIGH (26..70 us) */
    if (wait_level(1, DHT_WAIT_TIMEOUT_US) < 0) return ESP_ERR_TIMEOUT;  /* dây chưa được kéo lên */
    if (wait_level(0, DHT_WAIT_TIMEOUT_US) < 0) return ESP_ERR_TIMEOUT;  /* cảm biến kéo LOW */
    if (wait_level(1, DHT_WAIT_TIMEOUT_US) < 0) return ESP_ERR_TIMEOUT;  /* hết LOW 80 us */
    if (wait_level(0, DHT_WAIT_TIMEOUT_US) < 0) return ESP_ERR_TIMEOUT;  /* hết HIGH 80 us */

    for (int i = 0; i < 40; i++) {
        if (wait_level(1, DHT_BIT_TIMEOUT_US) < 0) return ESP_ERR_TIMEOUT;   /* hết LOW 50 us */
        int high_us = wait_level(0, DHT_BIT_TIMEOUT_US);                     /* đo độ rộng xung HIGH */
        if (high_us < 0) return ESP_ERR_TIMEOUT;
        data[i / 8] = (uint8_t)(data[i / 8] << 1);
        if (high_us > DHT_BIT_THRESHOLD_US) {
            data[i / 8] |= 1;
        }
    }
    return ESP_OK;
}

esp_err_t dht_init(gpio_num_t pin, dht_type_t type)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,   /* ghi 0 = kéo LOW, ghi 1 = nhả (pull-up kéo lên) */
        .pull_up_en   = GPIO_PULLUP_ENABLE,          /* dự phòng; vẫn nên có điện trở 4.7-10k ngoài */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(pin, 1);

    s_pin  = pin;
    s_type = type;
    int64_t min_interval_us = (type == DHT_MODEL_DHT22) ? 2000000 : 1000000;
    /* Sao cho lần đọc đầu tiên được phép sau đúng DHT_POWERUP_WAIT_US kể từ lúc init */
    s_last_us = esp_timer_get_time() - min_interval_us + DHT_POWERUP_WAIT_US;
    s_inited  = true;
    ESP_LOGI(TAG, "DHT%s on GPIO%d", (type == DHT_MODEL_DHT22) ? "22" : "11", (int)pin);
    return ESP_OK;
}

esp_err_t dht_read(float *temp_c, float *humidity_pct)
{
    if (!s_inited || temp_c == NULL || humidity_pct == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t min_interval_us = (s_type == DHT_MODEL_DHT22) ? 2000000 : 1000000;
    if (esp_timer_get_time() - s_last_us < min_interval_us) {
        return ESP_ERR_INVALID_STATE;                /* gọi quá sớm */
    }
    s_last_us = esp_timer_get_time();

    /* Bước 1: kéo LOW >= 18 ms bằng vTaskDelay (ngoài vùng găng, không busy-wait) */
    gpio_set_level(s_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(DHT_START_LOW_MS));

    /* Bước 2: nhả đường dây và lấy mẫu trong vùng găng (~4-5 ms, tắt ngắt) */
    uint8_t d[5];
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(s_pin, 1);
    esp_err_t err = read_frame(d);
    portEXIT_CRITICAL(&s_mux);

    gpio_set_level(s_pin, 1);                        /* luôn nhả đường dây */
    if (err != ESP_OK) {
        return err;
    }

    /* Bước 3: kiểm tra checksum */
    if ((uint8_t)(d[0] + d[1] + d[2] + d[3]) != d[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    /* Bước 4: giải mã */
    float hum, temp;
    if (s_type == DHT_MODEL_DHT22) {
        hum  = (float)((d[0] << 8) | d[1]) / 10.0f;
        temp = (float)(((d[2] & 0x7F) << 8) | d[3]) / 10.0f;
        if (d[2] & 0x80) temp = -temp;
        if (temp < -40.0f || temp > 80.0f || hum < 0.0f || hum > 100.0f) return ESP_ERR_INVALID_RESPONSE;
    } else {
        hum  = (float)d[0] + (float)d[1] / 10.0f;
        temp = (float)d[2] + (float)(d[3] & 0x7F) / 10.0f;
        if (d[3] & 0x80) temp = -temp;
        if (temp < 0.0f || temp > 50.0f || hum < 20.0f || hum > 90.0f) return ESP_ERR_INVALID_RESPONSE;
    }

    *temp_c = temp;
    *humidity_pct = hum;
    return ESP_OK;
}