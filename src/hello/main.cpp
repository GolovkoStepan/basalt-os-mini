// Самая маленькая прошивка: доказать, что процесс «собрал -> залил -> плата
// стартовала -> я это вижу» работает от начала до конца.
//
// Рассчитана на голый модуль M5StampS3 вне корпуса Cardputer: не трогает ни
// дисплей, ни SPI, ни клавиатуру. Периферии на столе нет, поэтому любая
// зависимость от неё только маскировала бы результат.
//
// Три независимых канала обратной связи, чтобы один отказ не прятал другой:
//   1. Serial - heartbeat каждые 500 мс, метка сборки и счётчик загрузок;
//   2. RGB LED на G21 - мигает цветами, живёт вообще без USB;
//   3. echo - что набрали, то и вернулось (проверка приёма, а не только передачи).
//
// Счётчик загрузок лежит в RTC-памяти: он выживает при сбросе, поэтому
// «boot=1» значит «первый запуск после прошивки», а растущий boot при
// сбрасывающемся uptime - самопроизвольные перезагрузки.

#include <Arduino.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "board_pins.h"

// Метка сборки: главный инструмент проверки «залилось ли именно новое».
// Если в логе старое время - заливка не дошла, и искать надо не в коде.
static const char kBuild[] = __DATE__ " " __TIME__;

#define BOOT_MAGIC 0xBA5A17u

RTC_NOINIT_ATTR static uint32_t g_magic;
RTC_NOINIT_ATTR static uint32_t g_boots;

static void count_boot(void)
{
    if (g_magic != BOOT_MAGIC) {
        g_magic = BOOT_MAGIC;
        g_boots = 0;
    }
    ++g_boots;
}

static const char* reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "ПАНИКА";
    case ESP_RST_INT_WDT:  return "watchdog (interrupt)";
    case ESP_RST_TASK_WDT: return "watchdog (task)";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    case ESP_RST_BROWNOUT: return "просадка питания";
    case ESP_RST_SDIO:     return "SDIO";
    case ESP_RST_USB:      return "USB";
    case ESP_RST_JTAG:     return "JTAG";
    default:               return "иное";
    }
}

// LED на G21. На Cardputer его питание завязано на линию подсветки (G38),
// поэтому G38 поднимаем заранее: на голом модуле это безвредно, а если
// питание действительно завязано - без этого LED не загорится.
static void led_init(void)
{
    pinMode(PIN_DISP_BL, OUTPUT);
    digitalWrite(PIN_DISP_BL, HIGH);
    rgbLedWrite(PIN_RGB_LED_G21, 0, 0, 0);
}

void setup()
{
    // Serial - первым делом, до всего остального. Прошлая версия инициализировала
    // LED раньше консоли, и любое зависание в LED-коде давало полную тишину:
    // непонятно, то ли прошивка не залилась, то ли упала, то ли плата мертва.
    // Консоль, поднятая раньше периферии, оставляет след о любой поломке.
    Serial.begin(115200);

    // Пауза, пока хост откроет порт: иначе первые строки уходят в никуда.
    delay(1500);

    count_boot();

    esp_chip_info_t info;
    esp_chip_info(&info);

    Serial.println();
    Serial.println("#################################");
    Serial.println("# basalt-os hello");
    Serial.printf("# сборка : %s\n", kBuild);
    Serial.printf("# загрузка: %lu, reset: %s (%d)\n", (unsigned long)g_boots,
                  reset_reason_str(esp_reset_reason()), (int)esp_reset_reason());
    Serial.println("#################################");
    Serial.printf("chip  : ESP32-S3, %d ядер, rev v%d.%d\n", info.cores,
                  info.revision / 100, info.revision % 100);
    Serial.printf("flash : %lu МБ\n",
                  (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
    Serial.printf("psram : %s\n", ESP.getPsramSize() ? "есть" : "нет");
    Serial.printf("heap  : %lu Б\n",
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // Метки до и после каждого шага: если лог оборвётся на «led init», ясно,
    // что зависло именно там, а не «плата сломалась».
    Serial.print("led init... ");
    Serial.flush();
    led_init();
    Serial.println("ок");

    Serial.println();
    Serial.println("дальше: heartbeat каждые 500 мс, LED мигает, ввод возвращается");
    Serial.println();
}

void loop()
{
    static int64_t next_hb = 0;
    static int64_t next_led = 0;
    static uint32_t hb_count = 0;
    static uint8_t led_step = 0;

    const int64_t now = esp_timer_get_time();

    if (now >= next_hb) {
        next_hb = now + 500000;
        Serial.printf("[hb %5lu] uptime %6lu мс, heap %lu Б, boot %lu\n",
                      (unsigned long)++hb_count,
                      (unsigned long)(now / 1000),
                      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                      (unsigned long)g_boots);
    }

    // LED мигает раз в секунду: видно с другого конца стола и без монитора.
    // Порядок красный -> зелёный -> синий заодно проверяет, не перепутаны ли
    // каналы (у WS2812 они идут GRB).
    if (now >= next_led) {
        next_led = now + 1000000;
        static const uint8_t kColors[3][3] = {
            {64, 0, 0}, {0, 64, 0}, {0, 0, 64},
        };
        const uint8_t* c = kColors[led_step % 3];
        rgbLedWrite(PIN_RGB_LED_G21, c[0], c[1], c[2]);
        ++led_step;
    }

    while (Serial.available() > 0) {
        const int ch = Serial.read();
        if (ch == 'r' || ch == 'R') {
            Serial.println("[cmd] перезагрузка по команде");
            delay(50);
            ESP.restart();
        }
        Serial.printf("[echo] '%c' (0x%02X)\n", (ch >= 32 && ch < 127) ? ch : '?',
                      (unsigned)(ch & 0xFF));
    }

    delay(10);
}
