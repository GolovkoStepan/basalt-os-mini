// Свой обработчик исключений. Зачем свой - см. panic.h.

#include "panic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "format.h"
#include "indicator.h"
#include "tick.h"

#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/timer_group_reg.h"

// Смещения полей кадра. Дублируются в vectors.S, потому что ассемблер не умеет
// читать C-структуру. Проверка ниже - единственное, что удерживает эти две
// раскладки от расхождения: ошибка здесь стоит часа отладки на живой плате.
_Static_assert(offsetof(basalt_frame_t, a0) == 0, "кадр: a0");
_Static_assert(offsetof(basalt_frame_t, a1) == 4, "кадр: a1");
_Static_assert(offsetof(basalt_frame_t, a2) == 8, "кадр: a2");
_Static_assert(offsetof(basalt_frame_t, a3) == 12, "кадр: a3");
_Static_assert(offsetof(basalt_frame_t, a7) == 28, "кадр: a7");
_Static_assert(offsetof(basalt_frame_t, a15) == 60, "кадр: a15");
_Static_assert(offsetof(basalt_frame_t, ps) == 64, "кадр: ps");
_Static_assert(offsetof(basalt_frame_t, pc) == 68, "кадр: pc");
_Static_assert(offsetof(basalt_frame_t, exccause) == 72, "кадр: exccause");
_Static_assert(offsetof(basalt_frame_t, excvaddr) == 76, "кадр: excvaddr");
_Static_assert(offsetof(basalt_frame_t, sar) == 80, "кадр: sar");
_Static_assert(offsetof(basalt_frame_t, vector) == 84, "кадр: vector");
_Static_assert(offsetof(basalt_frame_t, depc) == 88, "кадр: depc");
_Static_assert(sizeof(basalt_frame_t) == 92, "кадр: размер");

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
    // 4 - прерывание уровня 1. В норме оно обрабатывается в обработчике
    // уровня 1 и сюда не доходит; попадание означает, что обработчик не смог
    // снять источник (например, CCOMPARE0 остался позади CCOUNT).
    case 4: return "прерывание уровня 1 (обработчик не снял источник)";
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
    // 28 и 29 - то, что в C случается чаще всего: разыменованный NULL и
    // указатель на освобождённую или чужую память. Без этих двух строк самая
    // обычная ошибка печаталась бы как «неизвестная», и паника переставала
    // быть подсказкой. Проверено на живой плате: `crash` даёт именно 29.
    case 28: return "чтение по недопустимому адресу";
    case 29: return "запись по недопустимому адресу";
    default: return 0;
    }
}

// Какой слот сработал - словами. Номер слота смысла не несёт: важно, пришёл
// сбой из исключения или из прерывания, потому что чинят их по-разному.
static const char* vector_text(uint32_t vector)
{
    switch (vector) {
    case 2: return "прерывание уровня 2";
    case 3: return "прерывание уровня 3";
    case 4: return "прерывание уровня 4";
    case 5: return "прерывание уровня 5";
    case 6: return "отладочное исключение";
    case 7: return "NMI";
    case 8: return "исключение ядра";
    case 9: return "исключение пользователя";
    case 10: return "двойное исключение (сбой внутри обработчика)";
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
static void reboot_now(void) __attribute__((noreturn));

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

void basalt_vec_note(basalt_frame_t* frame)
{
    // Ограничение на число строк: если сбой повторяется циклом (двойное
    // исключение внутри двойного), без него лог превращается в мешанину, по
    // которой ничего не прочитать.
    static uint32_t printed;
    if (printed >= 3) {
        return;
    }
    ++printed;

    // Строка нарочно короткая: она печатается до полного разбора, и если
    // разбор не переживёт сбой, в консоли останется хотя бы это.
    basalt_put("\n[сбой] слот ");
    basalt_put_u64(frame->vector);
    basalt_put(", причина ");
    basalt_put_u64(frame->exccause);
    basalt_put(", EPC1 0x");
    basalt_put_hex(frame->pc, 8);
    basalt_put(", EXCVADDR 0x");
    basalt_put_hex(frame->excvaddr, 8);
    basalt_put("\n");
}

void basalt_panic_exc(basalt_frame_t* frame)
{
    if (in_panic) {
        // Второе исключение внутри паники. Печатать уже нечем - состояние
        // неизвестно, - а рекурсия съест стек. Стоим: плата останется с
        // красным светодиодом, и это честный признак поломки.
        for (;;) {
        }
    }
    in_panic = true;

    // Индикатор первым делом: пока печатается лог, признак поломки уже виден.
    // На Cardputer это красный светодиод, на T-Display-S3 - включённая
    // подсветка: цвета у неё нет, и просить красный бессмысленно.
    if (basalt_indicator_ready()) {
        basalt_indicator_set(200, 0, 0);
    }

    basalt_put("\n\n!!!!!! PANIC !!!!!!\n");

    const uint32_t cause = frame->exccause;
    const char* const cause_name = cause_text(cause);
    basalt_put("причина ");
    basalt_put_u64(cause);
    basalt_put(": ");
    if (cause_name) {
        basalt_put(cause_name);
    } else {
        basalt_put("неизвестная");
    }
    basalt_put("\n");

    const char* const vector_name = vector_text(frame->vector);
    basalt_put("вектор  ");
    basalt_put_u64(frame->vector);
    basalt_put("  ");
    if (vector_name) {
        basalt_put(vector_name);
    } else {
        basalt_put("неизвестный слот");
    }
    basalt_put("\n");

    basalt_put("EPC1     0x");
    basalt_put_hex(frame->pc, 8);
    basalt_put("  адрес инструкции, на которой всё остановилось\n");

    // Двойное исключение - единственный случай, когда EPC1 не про эту
    // остановку: он остался от первого исключения, а сбой случился внутри
    // обработчика, и его адрес аппаратура положила в DEPC. Печатать один EPC1
    // значило бы показать чужую инструкцию - на плате это уже стоило поиска
    // причины в font_glyph, где её не было.
    if (frame->vector == BASALT_VECTOR_DOUBLE) {
        basalt_put("DEPC     0x");
        basalt_put_hex(frame->depc, 8);
        basalt_put("  адрес сбоя внутри обработчика (это искать)\n");
    }

    basalt_put("EXCVADDR 0x");
    basalt_put_hex(frame->excvaddr, 8);
    // Ноль здесь - не «нет адреса». Причина 29 с EXCVADDR 0 и есть
    // разыменованный NULL, то есть самый обычный случай; писать «не про адрес»
    // значило бы увести от правильного ответа. Поэтому подсказка зависит от
    // причины, а не от значения.
    if (frame->exccause == 28 || frame->exccause == 29 || frame->exccause == 13 ||
        frame->exccause == 15 || frame->exccause == 3) {
        basalt_put("  адрес, к которому было обращение\n");
    } else {
        basalt_put("  этой причиной не используется\n");
    }

    basalt_put("PS       0x");
    basalt_put_hex(frame->ps, 8);
    basalt_put("  SAR 0x");
    basalt_put_hex(frame->sar, 8);
    basalt_put("\n");

    // Состояние прерываний на момент разбора: если причина 4 (прерывание
    // уровня 1), по этим двум числам видно, какое именно прерывание просят
    // обработать. Читаем сейчас, а не из кадра: кадр их не сохраняет, а
    // разница в пару инструкций здесь ничего не решает.
    uint32_t intenable = 0;
    uint32_t interrupt = 0;
    __asm__ volatile("rsr %0, intenable" : "=r"(intenable));
    __asm__ volatile("rsr %0, interrupt" : "=r"(interrupt));
    basalt_put("INTENABLE 0x");
    basalt_put_hex(intenable, 8);
    basalt_put("  INTERRUPT 0x");
    basalt_put_hex(interrupt, 8);
    basalt_put("\n");

    // Окна: без этих двух чисел разбор паники в слоте переполнения окон
    // сводится к догадкам. WINDOWBASE говорит, какое окно физически видно,
    // WINDOWSTART - какие окна аппаратура считает живыми; обработчик
    // WindowOverflow ищет по маске кадр предка, и если окно, чьи регистры мы
    // только что загрузили, в маске не отмечено, он возьмёт «указатель стека»
    // из регистра, который им никогда не был.
    uint32_t windowbase = 0;
    uint32_t windowstart = 0;
    __asm__ volatile("rsr %0, windowbase" : "=r"(windowbase));
    __asm__ volatile("rsr %0, windowstart" : "=r"(windowstart));
    basalt_put("WINDOWBASE ");
    basalt_put_u64(windowbase & 0xFu);
    basalt_put("  WINDOWSTART 0x");
    basalt_put_hex(windowstart, 8);
    basalt_put("\n");

    // Регистры парами: столбиком это 16 строк, а читать их всё равно
    // осмысленно только рядом друг с другом.
    const uint32_t regs[16] = {
        frame->a0,  frame->a1,  frame->a2,  frame->a3,
        frame->a4,  frame->a5,  frame->a6,  frame->a7,
        frame->a8,  frame->a9,  frame->a10, frame->a11,
        frame->a12, frame->a13, frame->a14, frame->a15,
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
