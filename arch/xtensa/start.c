// Своя точка входа: перекрываем weak-функции IDF. См. start.h.

#include <stdint.h>

#include "start.h"

#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

// Точка входа из ассемблера (start.S): переносит стек и зовёт basalt_main.
void basalt_start(void) __attribute__((noreturn));

// Перекрываем weak-функцию IDF. Вызывается портом после инициализации кэша,
// MMU и BSS.
void start_cpu0(void);

// Ядро 1. У IDF эта функция ведёт в FreeRTOS-овское ожидание планировщика,
// который мы не запускаем: ядро крутилось бы в холостом цикле, греясь на
// 160 МГц и ничего не делая. Поэтому паркуем его в инструкции ожидания
// прерывания - она останавливает ядро до любого прерывания, а прерываний на
// этом ядре у нас нет.
void start_cpu_other_cores(void);

// Ключ снятия защиты с регистров RTC. Один и тот же на всех чипах ESP.
#define RTC_WDT_WRITE_KEY 0x50D83AA1u

// Выключить сторожевой таймер RTC.
//
// Загрузчик включает его на 9 секунд (CONFIG_BOOTLOADER_WDT_TIME_MS) и
// рассчитывает, что приложение будет его кормить или выключит. Кормит его
// esp_system, а выключает - та же инициализация IDF. У нас старт свой:
// esp_system не запускается, планировщика нет, кормить таймер некому.
//
// Проявляется это так: прошивка работает девять секунд и молча перезагружается,
// снова и снова. Причина в логе при этом названа USB_UART_CHIP_RESET, то есть
// неверно, и поиск уходит в USB - проверено на живой плате, отладка стоила
// вечера. Поэтому таймер выключается явно и на самом раннем шаге своего кода.
static void rtc_wdt_disable(void)
{
    WRITE_PERI_REG(RTC_CNTL_WDTWPROTECT_REG, RTC_WDT_WRITE_KEY);
    CLEAR_PERI_REG_MASK(RTC_CNTL_WDTCONFIG0_REG, RTC_CNTL_WDT_EN);
    WRITE_PERI_REG(RTC_CNTL_WDTWPROTECT_REG, 0);
}

void start_cpu0(void)
{
    rtc_wdt_disable();
    basalt_start();
}

void start_cpu_other_cores(void)
{
    for (;;) {
        __asm__ volatile("waiti 0");
    }
}
