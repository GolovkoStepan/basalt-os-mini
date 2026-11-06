// Стек: закраска и измерение. Как и зачем - см. stack.h.

#include "stack.h"

#include <stdbool.h>
#include <stdint.h>

// Границы стека из start.S. Объявлены там же, потому что 8 КБ задаются
// ассемблером: размер стека - свойство переезда указателя, а не константа в C.
extern uint8_t basalt_stack_bottom[];
extern uint8_t basalt_stack_top[];

#define PAINT_WORD 0xA5A5A5A5u

// Сколько байт у самого указателя стека не закрашиваем. Кадр текущей функции
// и вызывающих уже лежит выше sp, ниже - свободно; отступ страхует от
// соглашений о выравнивании и от того, что мы не знаем размер своего кадра.
// Цена отступа - до 256 Б в отчёте могут оказаться занятыми зря. Это верхняя
// граница, а не типичная цифра: отчёт нужен, чтобы увидеть «осталось мало», а
// не чтобы мерить с точностью до байта.
#define PAINT_BACKOFF 256u

static bool s_painted;

void basalt_stack_paint(void)
{
    uintptr_t sp;
    __asm__ volatile("mov %0, a1" : "=r"(sp));

    const uintptr_t bottom = (uintptr_t)basalt_stack_bottom;
    uintptr_t limit = sp - PAINT_BACKOFF;

    // Стек меньше отступа - не наш случай, но лучше ничего не делать, чем
    // закрасить всё вместе с живыми кадрами.
    if (limit <= bottom) {
        return;
    }
    limit &= ~(uintptr_t)3u;

    // Словами, а не байтами: 8 КБ побайтно - это 8192 итерации на старте,
    // которые ничего не стоят, но и незачем.
    for (uint32_t* p = (uint32_t*)bottom; (uintptr_t)p < limit; ++p) {
        *p = PAINT_WORD;
    }

    s_painted = true;
}

size_t basalt_stack_used(void)
{
    if (!s_painted) {
        return 0;
    }

    const uintptr_t top = (uintptr_t)basalt_stack_top;
    const uint32_t* p = (const uint32_t*)basalt_stack_bottom;

    // Идём снизу вверх до первого слова, в которое кто-то писал. Это и есть
    // самый глубокий момент: всё, что ниже, тогда было занято.
    while ((uintptr_t)p < top && *p == PAINT_WORD) {
        ++p;
    }

    return (size_t)(top - (uintptr_t)p);
}

size_t basalt_stack_size(void)
{
    return (size_t)(basalt_stack_top - basalt_stack_bottom);
}
