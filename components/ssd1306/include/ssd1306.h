/*
 * ssd1306 - Driver OLED 128x64 (SSD1306 hoac SH1106) tren driver I2C master moi.
 *
 * Moi ham ve chi ghi vao framebuffer (RAM). ssd1306_init(), ssd1306_set_contrast() va
 * ssd1306_flush() cham bus I2C -> NGUOI GOI phai giu i2c_mutex quanh cac ham nay.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64

esp_err_t ssd1306_init(uint8_t i2c_addr, bool sh1106);
esp_err_t ssd1306_flush(void);
void ssd1306_set_contrast(uint8_t contrast);

void ssd1306_clear(void);
void ssd1306_draw_pixel(int x, int y, bool on);
void ssd1306_draw_hline(int x, int y, int w, bool on);
void ssd1306_draw_rect(int x, int y, int w, int h, bool on);
void ssd1306_fill_rect(int x, int y, int w, int h, bool on);
void ssd1306_invert_region(int x, int y, int w, int h);

void ssd1306_draw_char_scaled(int x, int y, char c, int scale);
void ssd1306_draw_text_scaled(int x, int y, const char *s, int scale);
int  ssd1306_text_width(const char *s, int scale);

void ssd1306_draw_bitmap(int x, int y, int w, int h, const uint8_t *data);
