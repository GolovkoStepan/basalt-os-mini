// Таймерное прерывание уровня 3 на CCOMPARE1. Подробности - в int1.h и
// vectors.S (обработчик целиком на ассемблере).

#include "int1.h"

#include <stdint.h>

#include "sdkconfig.h"

#include "tick.h"

// Линия прерывания и порог PS.INTLEVEL, на который настроен обработчик.
//
// Линия 15 - это CCOMPARE1 по конфигурации чипа (XCHAL_TIMER1_INTERRUPT), а её
// уровень - 3. Пару «номер таймера / уровень» нельзя менять по отдельности:
// обработчик заканчивается инструкцией `rfi 3` и лежит в слоте 0x1C0, а слот
// задаётся именно уровнем. Ошибка здесь стоит молчащего сброса, поэтому числа
// выписаны рядом с объяснением, а не растворены по коду.
#define BASALT_INT_LINE   15u
#define BASALT_INT_LEVEL  3u

// Состояние, которое читает и правит обработчик. Не static: адрес лежит в
// пуле таблицы векторов (INT1_STATE_POOL), и линкер должен его сохранить.
//
// Обработчик НЕ пишет на стек прерванного кода, и это не оптимизация, а
// условие корректности: область ниже SP занята спиллом окон, которым
// пользуется аппаратура при переполнении окна. Проверено на плате: кадр в
// несколько слов на стеке прерванного кода затирал спилл по смещению [sp-12],
// после чего обработчик переполнения окна читал оттуда мусор вместо указателя
// кадра и падал с двойным исключением - в одном прогоне из трёх, то есть
// выглядело как случайность. Регистры обработчик прячет в EXCSAVE_4..6.
//
// Раскладка продублирована в vectors.S (INT1_ST_*), потому что ассемблер не
// умеет читать C-структуру. Проверка ниже - единственное, что удерживает две
// раскладки от расхождения: ошибка выглядит на плате как «тики не растут»,
// без единого сообщения сборки.
typedef struct {
    volatile uint32_t ticks;    // 0: сколько НАСТОЯЩИХ тиков прошло
    uint32_t period;            // 4: период в тактах ядра
    volatile uint32_t deadline; // 8: CCOUNT, когда должен наступить следующий тик
} basalt_int1_state_t;

_Static_assert(__builtin_offsetof(basalt_int1_state_t, ticks) == 0,
               "состояние тика: ticks");
_Static_assert(__builtin_offsetof(basalt_int1_state_t, period) == 4,
               "состояние тика: period");
_Static_assert(__builtin_offsetof(basalt_int1_state_t, deadline) == 8,
               "состояние тика: deadline");

basalt_int1_state_t basalt_int1_state;

// Частота ядра. CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ - это то, что загрузчик
// действительно выставил; предполагать другое нельзя: на этой плате стоит
// 160 МГц (sdkconfig.defaults), и ошибка дала бы тик не той частоты.
#define BASALT_CPU_HZ ((uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u)

// Период в тактах ядра. Считается делением, а не магическим числом: при смене
// частоты или BASALT_INT1_HZ число должно поехать само.
static uint32_t period_cycles(void)
{
    return BASALT_CPU_HZ / BASALT_INT1_HZ;
}

void basalt_int1_init(void)
{
    // Счётчик тиков начинается не с нуля, а с числа периодов, прошедших с
    // запуска системного счётчика.
    //
    // Иначе `top` показывает противоречие: время идёт с `basalt_tick_init`
    // (самое начало старта), а тики - только с этого вызова, который стоит
    // после двухсекундной паузы для хоста. На плате это выглядело как
    // «uptime 3 с, тиков 55 (100 Гц)» - оба числа верны, но вместе читаются
    // как ошибка.
    //
    // Так счётчик означает то, что написано: сколько периодов по 10 мс прошло
    // с запуска. Не все из них прерывали процессор - во время паузы тика ещё
    // не было, - но время текло, и счётчик времени обязан это учитывать.
    basalt_int1_state.ticks =
        (uint32_t)(basalt_tick_cycles() * BASALT_INT1_HZ / BASALT_TICK_HZ);
    basalt_int1_state.period = period_cycles();

    // Первое срабатывание - через период от текущего значения. Без этой записи
    // CCOMPARE1 равен нулю, а CCOUNT давно ушёл вперёд: запись в прошлое на
    // Xtensa не даёт прерывания вообще (таймер срабатывает на равенстве), и
    // тик не пришёл бы никогда.
    uint32_t ccount = 0;
    __asm__ volatile("rsr %0, ccount" : "=r"(ccount));
    basalt_int1_state.deadline = ccount + basalt_int1_state.period;
    __asm__ volatile("wsr %0, ccompare1\n\t"
                     "esync" ::"r"(basalt_int1_state.deadline)
                     : "memory");

    // Разрешить линию 15 в контроллере прерываний ядра.
    //
    // Через чтение-или-запись INTENABLE, а не через `wsr intset`: в XEA2 intset
    // - это алиас регистра INTERRUPT, а он только для чтения, и запись в него
    // молча игнорируется. Проверено на плате: `wsr intset` собрался и
    // выполнился, а бит в INTENABLE остался нулевым - линия просила
    // обслуживания (INTERRUPT != 0), но обработчик не вызывался.
    uint32_t intenable = 0;
    __asm__ volatile("rsr %0, intenable" : "=r"(intenable));
    intenable |= 1u << BASALT_INT_LINE;
    __asm__ volatile("wsr %0, intenable\n\t"
                     "rsync" ::"r"(intenable)
                     : "memory");

    // Опустить порог ниже уровня прерывания: пока PS.INTLEVEL >= 3, тик
    // замаскирован. rsil делает «прочитать старое PS и записать новый уровень»
    // одной инструкцией, поэтому между двумя действиями нет окна, в котором
    // прерывание уже разрешено, а стек ещё не готов.
    //
    // Это же место - точка, после которой система становится асинхронной: всё,
    // что ниже в basalt_main, может быть прервано в любой инструкции. Поэтому
    // вызывается последним при старте, когда драйверы готовы.
    //
    // Ограничение "n", а не "I": у GCC буква "I" означает «непосредственное
    // значение в диапазоне», у clang такого ограничения нет вообще, и clangd
    // ругается `invalid input constraint 'I'`. "n" понимают оба компилятора -
    // это любое целое, известное на этапе компиляции, - и rsil получает
    // литерал, как того требует инструкция.
    __asm__ volatile("rsil %0, %1" : "=r"(ccount) : "n"(BASALT_INT_LEVEL - 1)
                     : "memory");
}

uint32_t basalt_int1_ticks(void)
{
    return basalt_int1_state.ticks;
}

uint32_t basalt_int1_period(void)
{
    return basalt_int1_state.period;
}

uint32_t basalt_int1_cpu_hz(void)
{
    return BASALT_CPU_HZ;
}

void basalt_int1_view(basalt_int1_state_view_t* out)
{
    __asm__ volatile("rsr %0, intenable" : "=r"(out->intenable));
    __asm__ volatile("rsr %0, interrupt" : "=r"(out->interrupt));
    __asm__ volatile("rsr %0, ps" : "=r"(out->ps));
    __asm__ volatile("rsr %0, ccount" : "=r"(out->ccount));
    __asm__ volatile("rsr %0, ccompare1" : "=r"(out->ccompare));
}

// Счётчик вложенности маски. Держится отдельно от маски, чтобы hold/release
// можно было звать парой из разных мест и не думать о том, кто первый: маска
// восстанавливается только на нуле, поэтому вложенный вызов внутри операции с
// flash не включит линию раньше времени.
static uint32_t s_hold_depth;

void basalt_int1_hold(void)
{
    if (s_hold_depth++ != 0) {
        return;
    }

    // Линия гасится чтением-или-записью INTENABLE, а не через `rsil`: rsil
    // работает с INTLEVEL и на время записи во flash погасил бы все прерывания
    // ниже уровня, а не одну нашу линию. Нужно ровно одно: чтобы тик не пришёл,
    // пока кэш выключен и обработчик не может исполняться из flash.
    uint32_t intenable = 0;
    __asm__ volatile("rsr %0, intenable" : "=r"(intenable));
    __asm__ volatile("wsr %0, intenable\n\t"
                     "rsync" ::"r"(intenable & ~(1u << BASALT_INT_LINE))
                     : "memory");
}

void basalt_int1_release(void)
{
    if (s_hold_depth == 0) {
        return;
    }
    if (--s_hold_depth != 0) {
        return;
    }

    uint32_t intenable = 0;
    __asm__ volatile("rsr %0, intenable" : "=r"(intenable));
    __asm__ volatile("wsr %0, intenable\n\t"
                     "rsync" ::"r"(intenable | (1u << BASALT_INT_LINE))
                     : "memory");
}

void basalt_int1_trigger(void)
{
    // Заказ тика: «приди сейчас». Периодический срок при этом НЕ трогается.
    //
    // CCOMPARE1 ставится чуть вперёд, а не назад: на Xtensa таймер срабатывает
    // на РАВЕНСТВЕ CCOUNT и CCOMPARE, поэтому запись в прошлое не даёт
    // прерывания вообще. Здесь была ошибка, которая стоила часа: писалось
    // `ccount - 1`, заказ не срабатывал, и тик шёл только когда запись случайно
    // попадала точно в счётчик - на плате это выглядело как «из них по тику 1»
    // и замерший счётчик.
    //
    // Запас (64 такта, 400 нс на 160 МГц) нужен потому, что между чтением CCOUNT
    // и записью проходит десяток инструкций: значение должно быть ещё в будущем.
    //
    // Периодический срок (`deadline`) остаётся в `basalt_int1_state`: обработчик
    // вернёт на него CCOMPARE1, распознав заказ по «CCOUNT ещё не дошёл до
    // срока». Иначе заказы, которых бывает тысячи в секунду, перекрывали бы
    // настоящий тик 100 Гц и счётчик тиков стоял бы на месте.
    //
    // Если до срока осталось меньше, чем `soon`, ставим срок - но только если
    // он ещё в будущем. Просроченный срок ставить нельзя: запись в прошлое не
    // даёт прерывания вообще (таймер срабатывает на равенстве), и заказ
    // потеряется. Просроченный срок обработает ближайший заказ: обработчик
    // увидит «CCOUNT >= deadline» и засчитает настоящий тик.
    const uint32_t soon = 64;
    uint32_t ccount = 0;
    __asm__ volatile("rsr %0, ccount" : "=r"(ccount));

    uint32_t when = ccount + soon;
    const int32_t before_deadline = (int32_t)(basalt_int1_state.deadline - ccount);
    const int32_t before_soon = (int32_t)(basalt_int1_state.deadline - when);
    if (before_deadline > 0 && before_soon < 0) {
        when = basalt_int1_state.deadline;
    }

    __asm__ volatile("wsr %0, ccompare1\n\t"
                     "esync" ::"r"(when)
                     : "memory");
}
