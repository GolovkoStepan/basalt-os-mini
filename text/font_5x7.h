// Сгенерировано tools/make_font.py - НЕ ПРАВИТЬ ВРУЧНУЮ.
// Шрифт 5x7: ASCII 0x20..0x7E, кириллица 0x410..0x44F, плюс Ё и ё.
// 7 байт на глиф, по байту на строку.
// Старший бит строки (0x10) - левый пиксель, 0x01 - правый.
//
// Менять форму буквы - в генераторе, здесь только результат.

#pragma once

#include <stdint.h>

#define FONT_GLYPH_W     5
#define FONT_GLYPH_H     7
#define FONT_ASCII_FIRST 0x20
#define FONT_ASCII_LAST  0x7E
#define FONT_CYR_FIRST   0x0410
#define FONT_CYR_LAST    0x044F
#define FONT_GLYPH_COUNT 161

// Глиф по коду символа или NULL, если такого глифа нет.
//
// Возвращается указатель на FONT_GLYPH_H байт, а не копия: таблицы в
// font_5x7.c и живут во flash, копировать их в стек незачем.
const uint8_t* font_glyph(uint32_t cp);

