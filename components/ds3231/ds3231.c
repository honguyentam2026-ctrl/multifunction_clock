#include "ds3231.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "i2c_bus.h"

#define DS3231_ADDR          0x68
#define DS3231_SCL_HZ        400000
#define DS3231_XFER_TIMEOUT_MS 50

#define REG_SECONDS          0x00   /* 0x00..0x06: giây, phút, giờ, thứ, ngày, tháng, năm (BCD) */
#define REG_CONTROL          0x0E   /* bit7 = EOSC (1 = dừng dao động khi chạy bằng pin) */
#define REG_STATUS           0x0F   /* bit7 = OSF */
#define REG_TEMP_MSB         0x11   /* 0x11: phần nguyên (có dấu), 0x12: bit7:6 = phần thập phân */
#define CONTROL_EOSC         0x80
#define STATUS_OSF           0x80
#define HOUR_12H_BIT         0x40

static const char *TAG = "RTC";
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_power_lost_at_init = false;

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, DS3231_XFER_TIMEOUT_MS);
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof(b), DS3231_XFER_TIMEOUT_MS);
}

static bool is_leap(uint16_t y)
{
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

uint8_t ds3231_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) {
        return 31;
    }
    if (month == 2 && is_leap(year)) {
        return 29;
    }
    return dim[month - 1];
}

uint8_t ds3231_calc_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    /* Thuật toán Sakamoto: kết quả 0=Chủ nhật..6=Thứ 7 */
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year;
    if (month < 3) {
        y -= 1;
    }
    int dow = (y + y / 4 - y / 100 + y / 400 + t[(month - 1) % 12] + day) % 7;
    return (uint8_t)((dow + 6) % 7);   /* đổi sang 0=Thứ 2 .. 6=Chủ nhật */
}

/* Lấy giờ build từ __DATE__ ("Oct  1 2026") và __TIME__ ("12:34:56") làm giờ mặc định */
static void build_time(rtc_time_t *t)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = "Jan";
    int d = 1, y = 2025, hh = 0, mm = 0, ss = 0;

    sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);

    const char *p = strstr(months, mon);
    int m = (p != NULL) ? (int)((p - months) / 3) + 1 : 1;

    t->year = (uint16_t)y;
    t->month = (uint8_t)m;
    t->day = (uint8_t)d;
    t->hour = (uint8_t)hh;
    t->minute = (uint8_t)mm;
    t->second = (uint8_t)ss;
    t->weekday = ds3231_calc_weekday(t->year, t->month, t->day);
}

esp_err_t ds3231_init(void)
{
    s_power_lost_at_init = false;
    if (s_dev == NULL) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = DS3231_ADDR,
            .scl_speed_hz    = DS3231_SCL_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(i2c_bus_get_handle(), &cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    uint8_t status = 0;
    esp_err_t err = read_regs(REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DS3231 not responding: %s", esp_err_to_name(err));
        return err;
    }
    s_power_lost_at_init = (status & STATUS_OSF) != 0;

    /* Chẩn đoán trạng thái RTC trước khi quá trình khởi tạo có thể ghi thay đổi. */
    uint8_t ctrl = 0;
    uint8_t raw[7] = { 0 };
    if (read_regs(REG_CONTROL, &ctrl, 1) == ESP_OK &&
        read_regs(REG_SECONDS, raw, sizeof(raw)) == ESP_OK) {
        ESP_LOGI(TAG, "power-up regs: status=0x%02X control=0x%02X time(BCD)=%02X:%02X:%02X date=%02X/%02X/%02X OSF=%d EOSC=%d",
                 (unsigned int)status, (unsigned int)ctrl,
                 (unsigned int)raw[2], (unsigned int)raw[1], (unsigned int)raw[0],
                 (unsigned int)raw[4], (unsigned int)raw[5], (unsigned int)raw[6],
                 (status & STATUS_OSF) ? 1 : 0, (ctrl & CONTROL_EOSC) ? 1 : 0);
    }

    if (status & STATUS_OSF) {
        ESP_LOGW(TAG, "OSF set: RTC lost power (CR2032 missing/dead/bad contact?) -> set default time = build time");
        rtc_time_t t;
        build_time(&t);
        err = ds3231_set_time(&t);
        if (err != ESP_OK) {
            return err;
        }
        err = write_reg(REG_STATUS, (uint8_t)(status & ~STATUS_OSF));
        if (err != ESP_OK) {
            return err;
        }
    }

    /* Ép chế độ 24h: nếu RTC đang ở 12h thì đọc (đã quy đổi) rồi ghi lại bằng 24h */
    uint8_t hour_reg = 0;
    err = read_regs(REG_SECONDS + 2, &hour_reg, 1);
    if (err == ESP_OK && (hour_reg & HOUR_12H_BIT)) {
        rtc_time_t t;
        if (ds3231_get_time(&t) == ESP_OK) {
            (void)ds3231_set_time(&t);
        }
    }

    ESP_LOGI(TAG, "DS3231 ready");
    return ESP_OK;
}

esp_err_t ds3231_get_time(rtc_time_t *t)
{
    if (s_dev == NULL || t == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t b[7];
    esp_err_t err = read_regs(REG_SECONDS, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t hour;
    if (b[2] & HOUR_12H_BIT) {                       /* chế độ 12h -> quy đổi sang 24h */
        hour = bcd2bin(b[2] & 0x1F);
        if (hour == 12) {
            hour = 0;
        }
        if (b[2] & 0x20) {                           /* bit PM */
            hour = (uint8_t)(hour + 12);
        }
    } else {
        hour = bcd2bin(b[2] & 0x3F);
    }

    rtc_time_t r;
    r.second = bcd2bin(b[0] & 0x7F);
    r.minute = bcd2bin(b[1] & 0x7F);
    r.hour   = hour;
    r.day    = bcd2bin(b[4] & 0x3F);
    r.month  = bcd2bin(b[5] & 0x1F);                 /* bỏ bit century (0x80) */
    r.year   = (uint16_t)(2000 + bcd2bin(b[6]));

    if (r.second > 59 || r.minute > 59 || r.hour > 23 ||
        r.month < 1 || r.month > 12 ||
        r.day < 1 || r.day > ds3231_days_in_month(r.year, r.month)) {
        return ESP_ERR_INVALID_RESPONSE;             /* dữ liệu rác (bus lỗi hoặc RTC chưa đặt giờ) */
    }
    r.weekday = ds3231_calc_weekday(r.year, r.month, r.day);

    *t = r;
    return ESP_OK;
}

esp_err_t ds3231_set_time(const rtc_time_t *t)
{
    if (s_dev == NULL || t == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (t->year < 2000 || t->year > 2099 || t->month < 1 || t->month > 12 ||
        t->day < 1 || t->day > ds3231_days_in_month(t->year, t->month) ||
        t->hour > 23 || t->minute > 59 || t->second > 59) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t wd = (uint8_t)(ds3231_calc_weekday(t->year, t->month, t->day) + 1);   /* thanh ghi: 1..7 */
    uint8_t b[8] = {
        REG_SECONDS,
        bin2bcd(t->second),
        bin2bcd(t->minute),
        bin2bcd(t->hour),                  /* bit6 = 0 -> chế độ 24h */
        wd,
        bin2bcd(t->day),
        bin2bcd(t->month),
        bin2bcd((uint8_t)(t->year - 2000)),
    };
    return i2c_master_transmit(s_dev, b, sizeof(b), DS3231_XFER_TIMEOUT_MS);
}

esp_err_t ds3231_get_temperature(float *temp_c)
{
    if (s_dev == NULL || temp_c == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[2];
    esp_err_t err = read_regs(REG_TEMP_MSB, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    *temp_c = (float)(int8_t)b[0] + (float)(b[1] >> 6) * 0.25f;
    return ESP_OK;
}

bool ds3231_lost_power(void)
{
    uint8_t status = 0;
    if (s_dev == NULL || read_regs(REG_STATUS, &status, 1) != ESP_OK) {
        return false;
    }
    return (status & STATUS_OSF) != 0;
}

bool ds3231_power_lost_at_init(void)
{
    return s_power_lost_at_init;
}