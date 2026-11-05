// Работа с пином как с обычным выходом - по регистрам, без driver/gpio.
//
// Зачем отдельный файл: на ESP32-S3 пины 0..31 и 32..48 живут в разных наборах
// регистров (GPIO_* и GPIO_*1), и одним сдвигом на 38 бит нужный бит не
// адресовать - компилятор справедливо ругается на переполнение сдвига. Плюс
// пин по умолчанию может быть отдан другой периферии, и тогда сигнал с матрицы
// GPIO до ножки не доходит вообще: в Arduino это делал pinMode внутри себя.
//
// Почему inline, а не .c: функции по три строки и вызываются из горячих мест
// (мигание индикатора), лишний вызов тут ничего не даёт.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "soc/gpio_periph.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/soc.h"

// Переключить IO_MUX на функцию GPIO.
static inline void pin_as_gpio(unsigned pin)
{
    if (pin < SOC_GPIO_PIN_COUNT) {
        const uint32_t reg = GPIO_PIN_MUX_REG[pin];
        if (reg != 0) {
            REG_SET_FIELD(reg, MCU_SEL, PIN_FUNC_GPIO);
        }
    }
}

// Выставить уровень. Регистры W1TS/W1TC меняют только нужный бит, поэтому
// гонки с другими пинами нет и читать-менять-писать не требуется.
static inline void pin_gpio_write(unsigned pin, bool level)
{
    const uint32_t bit = 1u << (pin & 31u);
    if (pin < 32) {
        SET_PERI_REG_MASK(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, bit);
    } else {
        SET_PERI_REG_MASK(level ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG, bit);
    }
}

// Сделать пином-выходом и сразу выставить уровень.
static inline void pin_gpio_output(unsigned pin, bool level)
{
    const uint32_t bit = 1u << (pin & 31u);
    pin_as_gpio(pin);
    if (pin < 32) {
        SET_PERI_REG_MASK(GPIO_ENABLE_W1TS_REG, bit);
    } else {
        SET_PERI_REG_MASK(GPIO_ENABLE1_W1TS_REG, bit);
    }
    pin_gpio_write(pin, level);
}
