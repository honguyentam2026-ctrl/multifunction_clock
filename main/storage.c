#include "storage.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ds3231.h"
#include "app_config.h"

static const char *TAG = "NVS";
static nvs_handle_t s_nvs = 0;

#define SAVED_TIME_BLOB_SIZE 8U

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

static bool time_is_valid(const rtc_time_t *t)
{
    return t != NULL && t->year >= 2000 && t->year <= 2099 &&
           t->month >= 1 && t->month <= 12 &&
           t->day >= 1 && t->day <= ds3231_days_in_month(t->year, t->month) &&
           t->hour < 24 && t->minute < 60 && t->second < 60;
}

esp_err_t storage_load_time(rtc_time_t *t)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rtc_time_t loaded = {
        0
    };
    uint64_t packed = 0;
    esp_err_t err = nvs_get_u64(s_nvs, "last_time64", &packed);
    if (err == ESP_OK) {
        loaded.second = (uint8_t)(packed % 100U);
        packed /= 100U;
        loaded.minute = (uint8_t)(packed % 100U);
        packed /= 100U;
        loaded.hour = (uint8_t)(packed % 100U);
        packed /= 100U;
        loaded.day = (uint8_t)(packed % 100U);
        packed /= 100U;
        loaded.month = (uint8_t)(packed % 100U);
        packed /= 100U;
        loaded.year = (uint16_t)packed;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Đọc blob cũ; giờ mới được lưu riêng dưới key uint64 để không đổi kiểu key NVS. */
        size_t size = 0;
        err = nvs_get_blob(s_nvs, "last_time", NULL, &size);
        if (err != ESP_OK) {
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                log_missing("last_time");
            }
            return err;
        }
        if (size != SAVED_TIME_BLOB_SIZE) {
            ESP_LOGW(TAG, "saved time has unsupported legacy format");
            return ESP_ERR_NVS_NOT_FOUND;
        }
        uint8_t blob[SAVED_TIME_BLOB_SIZE];
        err = nvs_get_blob(s_nvs, "last_time", blob, &size);
        if (err != ESP_OK) {
            return err;
        }
        if (blob[0] != 1U) {
            ESP_LOGW(TAG, "saved time has unsupported legacy version");
            return ESP_ERR_NVS_NOT_FOUND;
        }
        loaded.year = (uint16_t)(((uint16_t)blob[1] << 8) | blob[2]);
        loaded.month = blob[3];
        loaded.day = blob[4];
        loaded.hour = blob[5];
        loaded.minute = blob[6];
        loaded.second = blob[7];
    } else {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            log_missing("last_time");
        }
        return err;
    }

    if (!time_is_valid(&loaded)) {
        ESP_LOGW(TAG, "saved time is invalid");
        return ESP_ERR_NVS_NOT_FOUND;
    }
    loaded.weekday = ds3231_calc_weekday(loaded.year, loaded.month, loaded.day);
    *t = loaded;
    ESP_LOGD(TAG, "loaded time %04d-%02d-%02d %02d:%02d:%02d",
             (int)loaded.year, (int)loaded.month, (int)loaded.day,
             (int)loaded.hour, (int)loaded.minute, (int)loaded.second);
    return ESP_OK;
}

esp_err_t storage_save_time(const rtc_time_t *t)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!time_is_valid(t)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t packed = t->year;
    packed = packed * 100U + t->month;
    packed = packed * 100U + t->day;
    packed = packed * 100U + t->hour;
    packed = packed * 100U + t->minute;
    packed = packed * 100U + t->second;
    esp_err_t err = nvs_set_u64(s_nvs, "last_time64", packed);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_commit(s_nvs);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "saved time %04d-%02d-%02d %02d:%02d:%02d",
                 (int)t->year, (int)t->month, (int)t->day,
                 (int)t->hour, (int)t->minute, (int)t->second);
    }
    return err;
}

esp_err_t storage_load_settings(uint8_t *brightness, uint8_t *dim_preset,
                                bool *unit_f, bool *beep_on)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness == NULL || dim_preset == NULL || unit_f == NULL || beep_on == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t bright = OLED_BRIGHTNESS_LEVEL_COUNT - 1U;
    uint8_t dim = 0;
    uint8_t fahrenheit = 0;
    uint8_t beep = ENABLE_BUTTON_BEEP ? 1U : 0U;
    esp_err_t err = nvs_get_u8(s_nvs, "bright", &bright);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        log_missing("bright");
        bright = OLED_BRIGHTNESS_LEVEL_COUNT - 1U;
    } else if (err != ESP_OK) {
        return err;
    } else if (bright >= OLED_BRIGHTNESS_LEVEL_COUNT) {
        ESP_LOGW(TAG, "invalid brightness setting %u, using default", (unsigned)bright);
        bright = OLED_BRIGHTNESS_LEVEL_COUNT - 1U;
    }

    err = nvs_get_u8(s_nvs, "dim_preset", &dim);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        log_missing("dim_preset");
        dim = 0;
    } else if (err != ESP_OK) {
        return err;
    } else if (dim >= AUTO_DIM_PRESET_COUNT) {
        ESP_LOGW(TAG, "invalid auto-dim preset %u, using OFF", (unsigned)dim);
        dim = 0;
    }

    err = nvs_get_u8(s_nvs, "unit_f", &fahrenheit);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        log_missing("unit_f");
        fahrenheit = 0;
    } else if (err != ESP_OK) {
        return err;
    } else if (fahrenheit > 1U) {
        ESP_LOGW(TAG, "invalid temperature unit setting, using Celsius");
        fahrenheit = 0;
    }

    err = nvs_get_u8(s_nvs, "beep_on", &beep);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        log_missing("beep_on");
        beep = ENABLE_BUTTON_BEEP ? 1U : 0U;
    } else if (err != ESP_OK) {
        return err;
    } else if (beep > 1U) {
        ESP_LOGW(TAG, "invalid button beep setting, using default");
        beep = ENABLE_BUTTON_BEEP ? 1U : 0U;
    }

    *brightness = bright;
    *dim_preset = dim;
    *unit_f = fahrenheit != 0U;
    *beep_on = beep != 0U;
    return ESP_OK;
}

esp_err_t storage_save_settings(uint8_t brightness, uint8_t dim_preset,
                                bool unit_f, bool beep_on)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness >= OLED_BRIGHTNESS_LEVEL_COUNT || dim_preset >= AUTO_DIM_PRESET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_set_u8(s_nvs, "bright", brightness);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(s_nvs, "dim_preset", dim_preset);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(s_nvs, "unit_f", unit_f ? 1U : 0U);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(s_nvs, "beep_on", beep_on ? 1U : 0U);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_commit(s_nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved settings: brightness %u, dim %u, unit %s, beep %s",
                 (unsigned)brightness, (unsigned)dim_preset,
                 unit_f ? "F" : "C", beep_on ? "ON" : "OFF");
    }
    return err;
}
