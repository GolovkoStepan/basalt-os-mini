// Свой обработчик исключений. Зачем свой - см. panic.h.

#include "panic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "format.h"
#include "led_ws2812.h"
#include "tick.h"

#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/timer_group_reg.h"

// Раскладку кадра задаёт ABI, а не мы: берём её из заголовка Xtensa, чтобы не
// разойтись с ассемблерным кодом, который этот кадр заполняет.
#include <xtensa_context.h>

// Таблица обработчиков исключений IDF: по указателю на каждую пару
// «причина, ядро». Раскладка задана в xtensa_intr_asm.S - индекс считается
// как причина * число ядер + ядро. На ESP32-S3 ядер два, причин 64.
#define PANIC_CAUSE_COUNT 64
#define PANIC_CORE_COUNT 2

// Тип обработчика задаём сами: таблица хранит указатели на функции с такой
// подписью, и это единственное, что нам нужно знать об её устройстве.
typedef void (*exc_handler_t)(XtExcFrame*);

extern exc_handler_t _xt_exception_table[];

// Пауза перед сбросом: чтобы красный успели заметить, а лог - дочитать.
#define PANIC_HOLD_US 3000000u

// Ключ к регистрам сторожевых таймеров. Значение задано железом и одинаково
// для MWDT и RWDT: без него запись в конфигурацию игнорируется.
#define WDT_WRITE_KEY 0x50D83AA1u

static volatile bool in_panic;

// Выключить сторожевые таймеры.
//
// Мы не запускаем FreeRTOS, поэтому сторожевой таймер IDF (TG1) обычно не
// включён, и пауза проходит без этого кода - проверено. Но состояние таймеров
// зависит от того, что было до нас, а пауза в три секунды не должна
// превращаться в сброс по WDT: тогда наш собственный путь сброса не
// исполняется и, значит, не проверяется. Это уже случалось, когда систему
// запускал IDF: причина сброса приходила TG1WDT_SYS_RST вместо нашего.
static void stop_watchdogs(void)
{
    // TG1 - сторожевой таймер прерываний, он срабатывает первым.
    WRITE_PERI_REG(TIMG_WDTWPROTECT_REG(1), WDT_WRITE_KEY);
    CLEAR_PERI_REG_MASK(TIMG_WDTCONFIG0_REG(1), TIMG_WDT_EN);

    // TG0 - сторожевой таймер задач.
    WRITE_PERI_REG(TIMG_WDTWPROTECT_REG(0), WDT_WRITE_KEY);
    CLEAR_PERI_REG_MASK(TIMG_WDTCONFIG0_REG(0), TIMG_WDT_EN);

    // RTC-таймер: у него таймаут заметно больше, но живым его оставлять
    // незачем - мы всё равно перезагружаемся через три секунды.
    WRITE_PERI_REG(RTC_CNTL_WDTWPROTECT_REG, WDT_WRITE_KEY);
    CLEAR_PERI_REG_MASK(RTC_CNTL_WDTCONFIG0_REG, RTC_CNTL_WDT_EN);
}

// Названия причин, которые реально встречаются. Числа - из документации
// Xtensa ISA; незнакомая причина печатается числом, без догадок.
static const char* cause_text(uint32_t cause)
{
    switch (cause) {
    case 0: return "неизвестная инструкция";
    case 1: return "syscall";
    case 2: return "ошибка при выборке инструкции";
    case 3: return "ошибка при чтении или записи данных";
    case 4: return "прерывание уровня 1 (сюда попадать не должно)";
    case 5: return "переполнение alloca";
    case 6: return "деление на ноль";
    case 7: return "некорректное значение PC";
    case 8: return "привилегированная инструкция";
    case 9: return "невыровненное обращение к памяти";
    case 12: return "ошибка данных при выборке инструкции";
    case 13: return "ошибка данных при обращении к памяти";
    case 14: return "ошибка адреса при выборке инструкции";
    case 15: return "ошибка адреса при обращении к памяти";
    case 16: return "промах TLB при выборке инструкции";
    case 17: return "двойное попадание TLB при выборке";
    case 18: return "нет прав на выборку инструкции";
    case 20: return "запрещённая выборка инструкции";
    case 24: return "промах TLB при обращении к памяти";
    case 25: return "двойное попадание TLB при обращении к памяти";
    case 26: return "нет прав на обращение к памяти";
    case 28: return "запрещённое обращение к памяти";
    default: return 0;
    }
}

// Печать «A12 0x00000000» с выравниванием по колонкам.
static void print_reg(uint32_t index, uint32_t value)
{
    basalt_put("A");
    basalt_put_u64(index);
    basalt_put(index < 10 ? "  0x" : " 0x");
    basalt_put_hex(value, 8);
}

// Сброс чипа.
//
// Через регистр, а не esp_restart: esp_restart требует поднятых сервисов IDF,
// а мы их не поднимаем. Бит только выставляется, остальные поля регистра не
// трогаем - там настройки аналоговой части, сбросить их в ноль было бы хуже
// самой паники.
static void reboot_now(void)
{
    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    for (;;) {
    }
}

// Пауза перед сбросом с предохранителем.
//
// Если счётчик времени не идёт (тик ещё не поднят или сломан), ждать нечего:
// иначе паника превратилась бы в вечное молчание - ровно то, от чего мы
// уходим. Поэтому «время стоит» - это повод немедленно перезагружаться.
static void hold_before_reset(void)
{
    const uint64_t until = basalt_uptime_us() + PANIC_HOLD_US;
    uint64_t seen = basalt_uptime_us();
    uint32_t stuck = 0;

    while (basalt_uptime_us() < until) {
        const uint64_t now = basalt_uptime_us();
        if (now != seen) {
            seen = now;
            stuck = 0;
        } else if (++stuck > 200000u) {
            return;
        }
    }
}

// Обработчик получает кадр, который ассемблерный код IDF уже заполнил: там
// лежат регистры на момент сбоя, PC и PS. Обратно мы не возвращаемся никогда.
static void panic_handler(XtExcFrame* frame)
{
    if (in_panic) {
        // Второе исключение внутри паники. Печатать уже нечем - состояние
        // неизвестно, - а рекурсия съест стек. Стоим: плата останется с
        // красным светодиодом, и это честный признак поломки.
        for (;;) {
        }
    }
    in_panic = true;

    // Красный первым делом: пока печатается лог, цвет уже виден.
    if (basalt_led_ready()) {
        basalt_led_set(200, 0, 0);
    }

    basalt_put("\n\n!!!!!! PANIC !!!!!!\n");

    const uint32_t cause = (uint32_t)frame->exccause;
    const char* const text = cause_text(cause);
    basalt_put("причина ");
    basalt_put_u64(cause);
    basalt_put(": ");
    if (text) {
        basalt_put(text);
    } else {
        basalt_put("неизвестная");
    }
    basalt_put("\n");

    basalt_put("EPC1     0x");
    basalt_put_hex((uint32_t)frame->pc, 8);
    basalt_put("  адрес инструкции, на которой всё остановилось\n");

    basalt_put("EXCVADDR 0x");
    basalt_put_hex((uint32_t)frame->excvaddr, 8);
    basalt_put("  адрес, к которому было обращение (0 - не про адрес)\n");

    basalt_put("PS       0x");
    basalt_put_hex((uint32_t)frame->ps, 8);
    basalt_put("  SAR 0x");
    basalt_put_hex((uint32_t)frame->sar, 8);
    basalt_put("\n");

    // Регистры парами: столбиком это 16 строк, а читать их всё равно
    // осмысленно только рядом друг с другом.
    const uint32_t regs[16] = {
        (uint32_t)frame->a0,  (uint32_t)frame->a1,  (uint32_t)frame->a2,
        (uint32_t)frame->a3,  (uint32_t)frame->a4,  (uint32_t)frame->a5,
        (uint32_t)frame->a6,  (uint32_t)frame->a7,  (uint32_t)frame->a8,
        (uint32_t)frame->a9,  (uint32_t)frame->a10, (uint32_t)frame->a11,
        (uint32_t)frame->a12, (uint32_t)frame->a13, (uint32_t)frame->a14,
        (uint32_t)frame->a15,
    };
    for (uint32_t i = 0; i < 16; i += 2) {
        print_reg(i, regs[i]);
        basalt_put("  ");
        print_reg(i + 1, regs[i + 1]);
        basalt_put("\n");
    }

    basalt_put("сброс через ");
    basalt_put_u64(PANIC_HOLD_US / 1000000u);
    basalt_put(" с\n");

    stop_watchdogs();
    hold_before_reset();
    reboot_now();
}

void basalt_panic_init(void)
{
    // Счётчик времени обработчику нужен для паузы перед сбросом, и поднять
    // его здесь дешевле, чем зависеть от порядка инициализации: паника может
    // случиться раньше, чем основной код дойдёт до тика.
    basalt_tick_init();

    for (int cause = 0; cause < PANIC_CAUSE_COUNT; ++cause) {
        for (int core = 0; core < PANIC_CORE_COUNT; ++core) {
            _xt_exception_table[cause * PANIC_CORE_COUNT + core] = panic_handler;
        }
    }
}
