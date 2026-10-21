// Своя точка входа: перекрываем weak-функции IDF. См. start.h.

#include <stdint.h>

#include "start.h"

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

void start_cpu0(void)
{
    basalt_start();
}

void start_cpu_other_cores(void)
{
    for (;;) {
        __asm__ volatile("waiti 0");
    }
}
