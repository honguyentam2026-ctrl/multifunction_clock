#include "ssd1306.h"

#include <string.h>
#include "esp_log.h"
#include "i2c_bus.h"
#include "font5x7.h"

#define OLED_SCL_HZ          400000
#define OLED_XFER_TIMEOUT_MS 100
#define OLED_PAGES           (SSD1306_HEIGHT / 8)
#define CTRL_CMD             0x00
#define CTRL_DATA            0x40

static const char *TAG = "OLED";

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_sh1106 = false;
static uint8_t s_fb[SSD1306_WIDTH * OLED_PAGES];
static uint8_t s_tx[1 + SSD1306_WIDTH];

static esp_err_t send_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[16];
    if (n > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(s_dev, buf, n + 1, OLED_XFER_TIMEOUT_MS);
}

esp_err_t ssd1306_init(uint8_t i2c_addr, bool sh1106)
{
    s_sh1106 = sh1106;

    if (s_dev == NULL) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = i2c_addr,
            .scl_speed_hz = OLED_SCL_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(i2c_bus_get_handle(), &cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    static const uint8_t init_seq[][3] = {
        {1, 0xAE, 0x00}, {2, 0xD5, 0x80}, {2, 0xA8, 0x3F},
        {2, 0xD3, 0x00}, {1, 0x40, 0x00}, {2, 0x8D, 0x14},
        {2, 0x20, 0x02}, {1, 0xA1, 0x00}, {1, 0xC8, 0x00},
        {2, 0xDA, 0x12}, {2, 0x81, 0xCF}, {2, 0xD9, 0xF1},
        {2, 0xDB, 0x40}, {1, 0xA4, 0x00}, {1, 0xA6, 0x00},
    };
    for (size_t i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
        esp_err_t err = send_cmds(&init_seq[i][1], init_seq[i][0]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init failed: %s (check wiring / address 0x3C or 0x3D)", esp_err_to_name(err));
            return err;
        }
    }

    ssd1306_clear();
    esp_err_t err = ssd1306_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "first flush failed: %s", esp_err_to_name(err));
        return err;
    }
    const uint8_t on = 0xAF;
    err = send_cmds(&on, 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s ready @0x%02X", s_sh1106 ? "SH1106" : "SSD1306", i2c_addr);
    }
    return err;
}

esp_err_t ssd1306_flush(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t col_low = s_sh1106 ? 0x02 : 0x00;
    for (int page = 0; page < OLED_PAGES; page++) {
        const uint8_t cmds[3] = { (uint8_t)(0xB0 | page), col_low, 0x10 };
        esp_err_t err = send_cmds(cmds, sizeof(cmds));
        if (err != ESP_OK) {
            return err;
        }
        s_tx[0] = CTRL_DATA;
        memcpy(&s_tx[1], &s_fb[page * SSD1306_WIDTH], SSD1306_WIDTH);
        err = i2c_master_transmit(s_dev, s_tx, sizeof(s_tx), OLED_XFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

void ssd1306_set_contrast(uint8_t contrast)
{
    if (s_dev == NULL) {
        return;
    }
    const uint8_t cmds[2] = { 0x81, contrast };
    (void)send_cmds(cmds, sizeof(cmds));
}

void ssd1306_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void ssd1306_draw_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
        return;
    }
    uint8_t *p = &s_fb[x + (y / 8) * SSD1306_WIDTH];
    uint8_t mask = (uint8_t)(1u << (y % 8));
    if (on) {
        *p |= mask;
    } else {
        *p &= (uint8_t)~mask;
    }
}

void ssd1306_draw_hline(int x, int y, int w, bool on)
{
    for (int i = 0; i < w; i++) {
        ssd1306_draw_pixel(x + i, y, on);
    }
}

void ssd1306_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int j = 0; j < h; j++) {
        ssd1306_draw_hline(x, y + j, w, on);
    }
}

void ssd1306_draw_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    ssd1306_draw_hline(x, y, w, on);
    ssd1306_draw_hline(x, y + h - 1, w, on);
    for (int j = 0; j < h; j++) {
        ssd1306_draw_pixel(x, y + j, on);
        ssd1306_draw_pixel(x + w - 1, y + j, on);
    }
}

void ssd1306_invert_region(int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int px = x + i;
            int py = y + j;
            if (px < 0 || px >= SSD1306_WIDTH || py < 0 || py >= SSD1306_HEIGHT) {
                continue;
            }
            s_fb[px + (py / 8) * SSD1306_WIDTH] ^= (uint8_t)(1u << (py % 8));
        }
    }
}

void ssd1306_draw_char_scaled(int x, int y, char c, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    if (c < FONT5X7_FIRST_CHAR || c > FONT5X7_LAST_CHAR) {
        c = '?';
    }
    const uint8_t *glyph = font5x7[c - FONT5X7_FIRST_CHAR];
    for (int col = 0; col < FONT5X7_WIDTH; col++) {
        for (int row = 0; row < 8; row++) {
            if (glyph[col] & (1u << row)) {
                ssd1306_fill_rect(x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

int ssd1306_text_width(const char *s, int scale)
{
    if (s == NULL || *s == '\0') {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    return (int)strlen(s) * 6 * scale - scale;
}

void ssd1306_draw_text_scaled(int x, int y, const char *s, int scale)
{
    if (s == NULL) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    for (; *s != '\0'; s++) {
        ssd1306_draw_char_scaled(x, y, *s, scale);
        x += 6 * scale;
    }
}

void ssd1306_draw_bitmap(int x, int y, int w, int h, const uint8_t *data)
{
    if (data == NULL) {
        return;
    }
    int bytes_per_col = (h + 7) / 8;
    for (int col = 0; col < w; col++) {
        for (int row = 0; row < h; row++) {
            uint8_t b = data[col * bytes_per_col + row / 8];
            if (b & (1u << (row % 8))) {
                ssd1306_draw_pixel(x + col, y + row, true);
            }
        }
    }
}
