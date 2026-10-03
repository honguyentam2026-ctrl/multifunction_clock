/*
 * font5x7 - Font ASCII 5x7 (ky tu 32..126) va cac icon 8x8 nho.
 * Dinh dang: moi ky tu = 5 byte, moi byte la 1 cot, bit0 = hang tren cung.
 */
#pragma once

#include <stdint.h>

#define FONT5X7_FIRST_CHAR  32
#define FONT5X7_LAST_CHAR   126
#define FONT5X7_WIDTH       5

extern const uint8_t font5x7[FONT5X7_LAST_CHAR - FONT5X7_FIRST_CHAR + 1][FONT5X7_WIDTH];

/* Icon 8x8: 8 byte, moi byte la 1 cot 8 px. */
extern const uint8_t icon_thermometer[8];
extern const uint8_t icon_bell[8];
