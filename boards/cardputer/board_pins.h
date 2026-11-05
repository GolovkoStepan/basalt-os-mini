// Всё, что зависит от платы M5Stack Cardputer (K132), собрано здесь.
// Источник данных - docs/board.md, раздел 3. Магических чисел GPIO в логике
// быть не должно: правим только этот файл.

#pragma once

// --- Дисплей ST7789V2 (SPI3_HOST, только запись: MISO не разведён) --------
#define PIN_DISP_BL    38   // подсветка, PWM; тот же пин питает WS2812
#define PIN_DISP_RST   33
#define PIN_DISP_DC    34
#define PIN_DISP_MOSI  35
#define PIN_DISP_SCK   36
#define PIN_DISP_CS    37

// Панель физически 135x240 в окне RAM 240x320. Рабочий поворот - landscape
// (rotation 1), размер 240x135.
#define DISP_WIDTH  240
#define DISP_HEIGHT 135

// MADCTL и смещения окна при rotation 1 не выведены из даташита, а сняты с
// исполнявшегося на этой плате кода M5GFX (Panel_LCD::setRotation +
// getMadCtl): MX|MV|MH|BGR = 0x6C, colstart 40, rowstart 53.
// rowstart не 52, как offset_y в портретной конфигурации: при MX адресация
// строк идёт с другого конца RAM.
#define DISP_MADCTL    0x6C
#define DISP_OFFSET_X  40   // прибавляется к x при CASET
#define DISP_OFFSET_Y  53   // прибавляется к y при RASET

// --- Клавиатура: матрица 8x7 через демультиплексор 74HC138 ---------------
// Три адресных линии выбирают строку, семь входов читаются с подтяжкой,
// нажатие = LOW.
#define PIN_KEY_ADDR_0  8
#define PIN_KEY_ADDR_1  9
#define PIN_KEY_ADDR_2  11
#define PIN_KEY_ROW_0   13
#define PIN_KEY_ROW_1   15
#define PIN_KEY_ROW_2   3
#define PIN_KEY_ROW_3   4
#define PIN_KEY_ROW_4   5
#define PIN_KEY_ROW_5   6
#define PIN_KEY_ROW_6   7

#define KEY_LINES 8
#define KEY_COLS  7

// --- Прочее ---------------------------------------------------------------
#define PIN_BAT_ADC 10   // делитель 2:1
#define PIN_BUTTON_G0 0  // BOOT и пользовательская кнопка

// WS2812 на самом модуле M5StampS3. У ESP32-S3 в этом корпусе LED не на
// штатном G48, а на G21 - вариант платы esp32-s3-devkitc-1 этого не знает,
// поэтому адрес задаётся явно.
#define PIN_RGB_LED_G21 21
