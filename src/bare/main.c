// A0, шаг 3: своя точка входа.
//
// Что здесь своего: вывод в USB Serial/JTAG (arch/xtensa/console_usb_jtag.c),
// счётчик времени (arch/xtensa/tick.c), WS2812 через RMT
// (arch/xtensa/led_ws2812.c), обработчик исключений (arch/xtensa/panic.c) и
// точка входа (arch/xtensa/start.S, start.c) - всё по регистрам периферии.
// Что пока чужое: линкер-скрипт штатный и таблица векторов тоже - своя таблица
// требует своего линкер-скрипта, см. JOURNAL.
//
// Проверка: в терминале идёт баннер, затем строка tick раз в секунду,
// и на каждый тик мигает зелёный светодиод.

#include <stdbool.h>
#include <stdint.h>

#include "console_usb_jtag.h"
#include "format.h"
#include "led_ws2812.h"
#include "panic.h"
#include "start.h"
#include "tick.h"

// Яркость индикатора. Полные 255 бьют по глазам в тёмной комнате, а задача
// светодиода - «видно с другого конца стола», а не «осветить стол».
#define LED_ALIVE_LEVEL 40

// ---------------------------------------------------------------------------

// Точка входа нашей системы. Вызывается из basalt_start (start.S) на своём
// стеке, уже после того, как порт IDF подготовил кэш, MMU и BSS. Возврата
// нет: ниже цикл, из которого не выходят.
void basalt_main(void)
{
    // Порядок инициализации - от самого нужного к самому зависимому.
    // Консоль первой: если что-то дальше зависнет, в логе останется след.
    basalt_console_init();
    basalt_tick_init();
    // Паника - сразу после консоли: дальше идёт работа с периферией, и любая
    // её ошибка должна попадать уже в наш обработчик.
    basalt_panic_init();
    basalt_led_init();

    // Пауза до первого вывода: после сброса хост ещё не открыл порт, и баннер
    // иначе теряется - видно только середину лога, что мешает проверке.
    basalt_delay_us(2000000);

    const uint64_t t_boot = basalt_uptime_us();

    basalt_put("\n\nbasalt-os boot\n");
    basalt_put("[arch] ESP32-S3, Xtensa LX7\n");
    basalt_put("[console] USB Serial/JTAG, свой драйвер, база 0x");
    basalt_put_hex(0x60038000u, 8);
    basalt_put("\n[tick] SYSTIMER Unit 0, 16 МГц\n");

    // Одной строкой - состояние светодиода при старте. Если он молчит,
    // первым делом надо знать, дошли ли кадры до передатчика и с какой
    // частотой они ушли: без этих двух чисел отладка начинается с нуля.
    basalt_put("[led] WS2812 на G21 через RMT, 10 МГц, порядок GRB, память ");
    basalt_put(basalt_led_memory_ok() ? "ok, кадр " : "НЕ ПИШЕТСЯ, кадр ");
    basalt_put_u64(basalt_led_frame_us());
    basalt_put(" мкс\n");

    basalt_put("[panic] свой обработчик исключений, 64 причины, 2 ядра\n");
    basalt_put("[start] своя точка входа: start_cpu0 → basalt_start, FreeRTOS не запускается\n");
    basalt_put("[start] чужое: call_start_cpu0 (кэш, MMU, BSS), линкер и таблица векторов\n");
    basalt_put("ready\n\n");

    uint32_t tick = 0;
    uint64_t next = t_boot + 1000000u;
    uint64_t led_off_at = 0;

    for (;;) {
        const uint64_t now = basalt_uptime_us();

#ifdef BASALT_PANIC_TEST
        // Намеренное исключение через 5 секунд после старта. Нужно, чтобы
        // убедиться: паника печатает регистры и перезагружает плату, а не
        // вешает её молча. Делитель читается из volatile, иначе компилятор
        // вправе вычислить деление на ноль на этапе сборки.
        static volatile int zero = 0;
        static bool panic_warned = false;
        if (now > t_boot + 5000000u && !panic_warned) {
            panic_warned = true;
            basalt_put("\n[panic-test] деление на ноль\n");
            volatile int bad = 1 / zero;
            (void)bad;
        }
#endif

        if (now >= next) {
            next += 1000000u;
            ++tick;

            // Номер тика и uptime рядом: пропуск номера или скачок uptime -
            // признак того, что цикл где-то блокируется.
            basalt_put("tick ");
            basalt_put_u64(tick);
            basalt_put("  uptime ");
            basalt_put_sec3(now);
            basalt_put(" s\n");

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
            basalt_put("[rx] ");
            basalt_put_u64(got);
            basalt_put(" Б: ");
            for (size_t i = 0; i < got; ++i) {
                const uint8_t c = buf[i];
                if (c >= 32 && c < 127) {
                    basalt_console_write(&c, 1);
                } else {
                    basalt_put("<");
                    basalt_put_hex(c, 2);
                    basalt_put(">");
                }
            }
            basalt_put("\n");
        }

        // Без задержки цикл крутится вхолостую и греет ядро на 240 МГц.
        basalt_delay_us(1000);
    }
}
