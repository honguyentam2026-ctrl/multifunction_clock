#include "i2c_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define I2C_PROBE_TIMEOUT_MS 50
#define I2C_LOCK_TIMEOUT_MS  200

static const char *TAG = "RTC";   /* dùng chung tag bus I2C/RTC cho dễ lọc log */

static i2c_master_bus_handle_t s_bus   = NULL;
static SemaphoreHandle_t       s_mutex = NULL;

esp_err_t i2c_bus_init(gpio_num_t sda, gpio_num_t scl)
{
    if (s_bus != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Mutex (có kế thừa độ ưu tiên) bảo vệ MỌI giao dịch I2C */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = sda,
        .scl_io_num        = scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* dự phòng nếu module thiếu điện trở kéo lên */
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C bus ready (SDA=%d, SCL=%d)", (int)sda, (int)scl);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return s_bus;
}

bool i2c_bus_lock(uint32_t timeout_ms)
{
    if (s_mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void i2c_bus_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t i2c_bus_probe(uint8_t addr)
{
    if (s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!i2c_bus_lock(I2C_LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "probe: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_probe(s_bus, addr, I2C_PROBE_TIMEOUT_MS);
    i2c_bus_unlock();
    return err;
}

int i2c_bus_scan(void)
{
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_bus_probe(addr) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "No I2C device found - check wiring (SDA/SCL/VCC/GND)");
    }
    return found;
}