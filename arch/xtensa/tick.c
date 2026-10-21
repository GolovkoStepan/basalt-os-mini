// Чтение системного таймера ESP32-S3 по регистрам.

#include "tick.h"

#include "soc/systimer_reg.h"

void basalt_tick_init(void)
{
    // CLK_EN - тактирование блока, UNIT0_WORK_EN - разрешение счёта.
    // Оба могут быть уже подняты загрузчиком; выставляем явно, чтобы не
    // зависеть от того, что там успело инициализироваться.
    SET_PERI_REG_MASK(SYSTIMER_CONF_REG,
                      SYSTIMER_CLK_EN | SYSTIMER_TIMER_UNIT0_WORK_EN);

    // Защёлкнуть текущее значение, чтобы первое чтение было валидным.
    SET_PERI_REG_MASK(SYSTIMER_UNIT0_OP_REG, SYSTIMER_TIMER_UNIT0_UPDATE);
    CLEAR_PERI_REG_MASK(SYSTIMER_UNIT0_OP_REG, SYSTIMER_TIMER_UNIT0_UPDATE);
}

uint64_t basalt_tick_cycles(void)
{
    SET_PERI_REG_MASK(SYSTIMER_UNIT0_OP_REG, SYSTIMER_TIMER_UNIT0_UPDATE);
    CLEAR_PERI_REG_MASK(SYSTIMER_UNIT0_OP_REG, SYSTIMER_TIMER_UNIT0_UPDATE);

    // Пока защёлкивание не завершилось, VALUE_VALID снят. Ждём его, иначе
    // можно прочитать старое значение или смесь старшего и младшего слова.
    uint32_t spin = 0;
    while ((READ_PERI_REG(SYSTIMER_UNIT0_OP_REG) &
            SYSTIMER_TIMER_UNIT0_VALUE_VALID) == 0) {
        if (++spin > 100000) {
            break;
        }
    }

    const uint64_t hi = READ_PERI_REG(SYSTIMER_UNIT0_VALUE_HI_REG);
    const uint64_t lo = READ_PERI_REG(SYSTIMER_UNIT0_VALUE_LO_REG);
    return (hi << 32) | lo;
}

uint64_t basalt_uptime_us(void)
{
    return basalt_tick_cycles() / (BASALT_TICK_HZ / 1000000u);
}

void basalt_delay_us(uint32_t us)
{
    // Планировщика нет, усыпить ядро нечем: ждём, крутясь на счётчике.
    // Отсюда ограничение - это ожидание, а не сон: ядро всё это время
    // работает, и ток потребления не падает.
    const uint64_t until = basalt_uptime_us() + us;
    while (basalt_uptime_us() < until) {
    }
}
