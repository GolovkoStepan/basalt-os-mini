// A0, шаг 1: старт без Arduino, своя консоль, свой тик, свой светодиод.
//
// Что здесь своего: вывод в USB Serial/JTAG (arch/xtensa/console_usb_jtag.c),
// счётчик времени (arch/xtensa/tick.c) и WS2812 через RMT
// (arch/xtensa/led_ws2812.c) - всё по регистрам периферии.
// Что пока чужое: app_main вызывает старт IDF, а не наш загрузчик, и
// линкер-скрипт штатный. Это следующий шаг вехи: пока консоль своя и
// работает, ошибку в линкере будет чем диагностировать.
//
// Проверка: в терминале идёт баннер, затем строка tick раз в секунду,
// и на каждый тик мигает зелёный светодиод.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console_usb_jtag.h"
#include "led_ws2812.h"
#include "tick.h"

// Яркость индикатора. Полные 255 бьют по глазам в тёмной комнате, а задача
// светодиода - «видно с другого конца стола», а не «осветить стол».
#define LED_ALIVE_LEVEL 40
#define LED_PANIC_LEVEL 120

// ---------------------------------------------------------------------------
// Форматирование. Полноценный printf тянет newlib и тянет за собой кучу кода
// ради %d; здесь нужны только строки, десятичное и hex.
// ---------------------------------------------------------------------------

static void put(const char* s)
{
    const char* p = s;
    while (*p) {
        ++p;
    }
    if (p != s) {
        basalt_console_write(s, (size_t)(p - s));
    }
}

static void put_u64(uint64_t v)
{
    char buf[21];
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    }
    while (v > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    put(&buf[i]);
}

static void put_hex(uint32_t v, int digits)
{
    static const char kDigits[] = "0123456789abcdef";
    char buf[9];
    for (int i = digits - 1; i >= 0; --i) {
        buf[i] = kDigits[v & 0xF];
        v >>= 4;
    }
    buf[digits] = '\0';
    put(buf);
}

// «Целая.три знака»: 1.000, 2.001. Отдельный формат нужен, чтобы подъём
// счётчика был виден глазом, а не сливался в одно число.
static void put_seconds(uint64_t us)
{
    put_u64(us / 1000000u);
    put(".");

    const uint32_t ms = (uint32_t)((us % 1000000u) / 1000u);
    char buf[4];
    buf[0] = (char)('0' + (ms / 100) % 10);
    buf[1] = (char)('0' + (ms / 10) % 10);
    buf[2] = (char)('0' + ms % 10);
    buf[3] = '\0';
    put(buf);
}

// Задержка опросом переехала в arch: она нужна и драйверу светодиода.

// ---------------------------------------------------------------------------

// Точка входа IDF. Сами в неё попадаем не через свою таблицу векторов, а
// через чужой старт - см. шапку файла.
void app_main(void)
{
    basalt_console_init();
    basalt_tick_init();
    basalt_led_init();

    // Пауза до первого вывода: после сброса хост ещё не открыл порт, и баннер
    // иначе теряется - видно только середину лога, что мешает проверке.
    basalt_delay_us(2000000);

    const uint64_t t_boot = basalt_uptime_us();

    put("\n\nbasalt-os boot\n");
    put("[arch] ESP32-S3, Xtensa LX7\n");
    put("[console] USB Serial/JTAG, свой драйвер, база 0x");
    put_hex(0x60038000u, 8);
    put("\n[tick] SYSTIMER Unit 0, 16 МГц\n");
    // Одной строкой - состояние светодиода при старте. Если он молчит,
    // первым делом надо знать, дошли ли кадры до передатчика и с какой
    // частотой они ушли: без этих двух чисел отладка начинается с нуля.
    put("[led] WS2812 на G21 через RMT, 10 МГц, порядок GRB, память ");
    put(basalt_led_memory_ok() ? "ok, кадр " : "НЕ ПИШЕТСЯ, кадр ");
    put_u64(basalt_led_frame_us());
    put(" мкс\n");
    put("[start] app_main через старт IDF, своя таблица векторов - далее\n");
    put("ready\n\n");

    uint32_t tick = 0;
    uint64_t next = t_boot + 1000000u;
    uint64_t led_off_at = 0;

    for (;;) {
        const uint64_t now = basalt_uptime_us();

#ifdef BASALT_PANIC_TEST
        // Намеренное исключение через 5 секунд после старта. Нужно, чтобы
        // убедиться: паника печатает регистры, а не подвешивает плату
        // молча. Делитель читается из volatile, иначе компилятор вправе
        // вычислить деление на ноль на этапе сборки.
        static volatile int zero = 0;
        static bool panic_warned = false;
        if (now > t_boot + 5000000u && !panic_warned) {
            panic_warned = true;
            // Красный до сброса: WS2812 защёлкивает цвет, и он остаётся
            // гореть, пока идёт дамп. По цвету видно, что это сбой, а не
            // обычная перезаливка.
            basalt_led_set(LED_PANIC_LEVEL, 0, 0);
            put("\n[panic-test] деление на ноль\n");
            volatile int bad = 1 / zero;
            (void)bad;
        }
#endif

        if (now >= next) {
            next += 1000000u;
            ++tick;

            // Номер тика и uptime рядом: пропуск номера или скачок uptime -
            // признак того, что цикл где-то блокируется.
            put("tick ");
            put_u64(tick);
            put("  uptime ");
            put_seconds(now);
            put(" s\n");

            // Вспышка на тик - признак жизни, который видно без терминала:
            // кабель отключён, порт закрыт, а плата работает.
            basalt_led_set(0, LED_ALIVE_LEVEL, 0);
            led_off_at = now + 80000u;
        }

        // Гасим по времени, а не задержкой: цикл продолжает обслуживать
        // консоль, пока светодиод горит.
        if (led_off_at != 0 && now >= led_off_at) {
            basalt_led_set(0, 0, 0);
            led_off_at = 0;
        }

        // Эхо принятых байт: доказывает, что канал двусторонний, а не только
        // на запись. На A0 это единственный способ проверить приём.
        uint8_t buf[32];
        const size_t got = basalt_console_read(buf, sizeof(buf));
        if (got > 0) {
            put("[rx] ");
            put_u64(got);
            put(" Б: ");
            for (size_t i = 0; i < got; ++i) {
                const uint8_t c = buf[i];
                if (c >= 32 && c < 127) {
                    basalt_console_write(&c, 1);
                } else {
                    put("<");
                    put_hex(c, 2);
                    put(">");
                }
            }
            put("\n");
        }

        // Без задержки цикл крутится вхолостую и греет ядро на 240 МГц.
        basalt_delay_us(1000);
    }
}
