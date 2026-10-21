// Минимальная прошивка для проверки, что плата жива: баннер в USB Serial/JTAG,
// тик и опрос клавиатуры. Ничего, что может уронить старт: ни SPI, ни DMA, ни
// смены частот. Задача этого env - восстановить связь и получить обратную
// связь от платы, а не собрать факты о железе.
//
// Запускать: pio run -e console -t upload, потом tools/serial-read.sh 10

#include <Arduino.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "board_pins.h"

static const uint8_t kKeyAddr[3] = {PIN_KEY_ADDR_0, PIN_KEY_ADDR_1, PIN_KEY_ADDR_2};
static const uint8_t kKeyRow[KEY_COLS] = {
    PIN_KEY_ROW_0, PIN_KEY_ROW_1, PIN_KEY_ROW_2, PIN_KEY_ROW_3,
    PIN_KEY_ROW_4, PIN_KEY_ROW_5, PIN_KEY_ROW_6,
};

static uint64_t keys_scan(void)
{
    uint64_t map = 0;
    for (int line = 0; line < KEY_LINES; ++line) {
        digitalWrite(PIN_KEY_ADDR_0, (line & 1) ? HIGH : LOW);
        digitalWrite(PIN_KEY_ADDR_1, (line & 2) ? HIGH : LOW);
        digitalWrite(PIN_KEY_ADDR_2, (line & 4) ? HIGH : LOW);
        delayMicroseconds(10);
        for (int col = 0; col < KEY_COLS; ++col) {
            if (digitalRead(kKeyRow[col]) == LOW) {
                map |= 1ULL << (line * KEY_COLS + col);
            }
        }
    }
    return map;
}

void setup()
{
    // Подсветку включаем раньше консоли: это единственный видимый признак
    // жизни, который не зависит от драйвера панели. Содержимое экрана при
    // этом мусорное - ST7789 после сброса не инициализирован, и так и должно
    // быть: яркий экран здесь означает «прошивка стартовала».
    ledcAttach(PIN_DISP_BL, 5000, 8);
    ledcWrite(PIN_DISP_BL, 80);

    delay(1200);
    Serial.begin(115200);
    delay(200);

    esp_chip_info_t info;
    esp_chip_info(&info);

    Serial.println();
    Serial.println("==========================================");
    Serial.println("basalt-os console: проверка связи");
    Serial.println("==========================================");
    Serial.printf("chip   : ESP32-S3, %d ядер, rev v%d.%d\n",
                  info.cores, info.revision / 100, info.revision % 100);
    Serial.printf("psram  : %s\n", ESP.getPsramSize() ? "есть" : "нет");
    Serial.printf("reset  : %d\n", (int)esp_reset_reason());
    Serial.printf("heap   : %lu Б свободно\n",
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    Serial.println("если видно это - плата жива, консоль работает");
    Serial.println();

    for (uint8_t p : kKeyAddr) {
        pinMode(p, OUTPUT);
        digitalWrite(p, LOW);
    }
    for (uint8_t p : kKeyRow) {
        pinMode(p, INPUT_PULLUP);
    }
    Serial.println("[keys] жду нажатий");
}

void loop()
{
    static uint64_t prev = 0;
    const uint64_t now = keys_scan();
    if (now != prev) {
        for (int i = 0; i < KEY_LINES * KEY_COLS; ++i) {
            if ((now ^ prev) & (1ULL << i)) {
                Serial.printf("[keys] %s строка %d столбец %d\n",
                              (now & (1ULL << i)) ? "нажата  " : "отпущена",
                              i / KEY_COLS, i % KEY_COLS);
            }
        }
        prev = now;
    }

    static int64_t next = 0;
    const int64_t t = esp_timer_get_time();
    if (t >= next) {
        next = t + 2000000;
        Serial.printf("[tick] %lu с, heap %lu Б\n", (unsigned long)(t / 1000000),
                      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    }

    delay(5);
}
