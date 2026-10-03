#include "storage.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "NVS";
static nvs_handle_t s_nvs = 0;

static void log_missing(const char *key)
{
    ESP_LOGI(TAG, "no saved %s, using defaults", key);
}

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open("clock_cfg", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t storage_load_alarm(uint8_t *hour, uint8_t *minute, bool *enabled)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t h = 7;
    uint8_t m = 0;
    uint8_t en = 0;
    esp_err_t err = nvs_get_u8(s_nvs, "alarm_h", &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        log_missing("alarm_h");
        err = ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u8(s_nvs, "alarm_m", &m);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        log_missing("alarm_m");
        err = ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u8(s_nvs, "alarm_en", &en);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        log_missing("alarm_en");
        err = ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    if (hour) {
        *hour = h;
    }
    if (minute) {
        *minute = m;
    }
    if (enabled) {
        *enabled = (en != 0);
    }
    return ESP_OK;
}

esp_err_t storage_save_alarm(uint8_t hour, uint8_t minute, bool enabled)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_u8(s_nvs, "alarm_h", hour);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(s_nvs, "alarm_m", minute);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(s_nvs, "alarm_en", enabled ? 1U : 0U);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_commit(s_nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved alarm %02d:%02d %s", hour, minute, enabled ? "ON" : "OFF");
    }
    return err;
}

esp_err_t storage_load_cd_total(uint32_t *total_s)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t value = 0;
    esp_err_t err = nvs_get_u32(s_nvs, "cd_total_s", &value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        log_missing("cd_total_s");
        if (total_s) {
            *total_s = 0;
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (total_s) {
        *total_s = value;
    }
    return ESP_OK;
}

esp_err_t storage_save_cd_total(uint32_t total_s)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_u32(s_nvs, "cd_total_s", total_s);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_nvs);
}
