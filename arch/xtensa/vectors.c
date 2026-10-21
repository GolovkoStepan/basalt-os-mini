// Установка своей таблицы векторов. Где она лежит и почему - см. vectors.S.

#include "vectors.h"

#include <stdbool.h>
#include <stdint.h>

#include "format.h"
#include "panic.h"

#include "soc/soc.h"

// Таблица собрана линкером в IRAM с выравниванием на 1 КБ - см. vectors.S.
extern const uint8_t basalt_vectors[];
extern const uint8_t basalt_vectors_end[];

// Смещение пула с адресом C-обработчика внутри таблицы - то же, что
// HANDLER_POOL в vectors.S.
#define HANDLER_POOL_OFFSET 0x3d8

static uint32_t s_vectors_base;

void basalt_vectors_init(void)
{
    const uint32_t base = (uint32_t)(uintptr_t)basalt_vectors;

    basalt_put("[vectors] таблица в IRAM 0x");
    basalt_put_hex(base, 8);
    basalt_put(", размер ");
    basalt_put_u64((uint32_t)(basalt_vectors_end - basalt_vectors));
    basalt_put(" Б\n");

    // Выравнивание проверяем, а не предполагаем: VECBASE молча отбросит
    // младшие 10 бит, и сбой потом искали бы по чужому адресу. Ошибка
    // сборки должна быть видна здесь, а не через полчаса в панике.
    if ((base & 0x3ffu) != 0) {
        basalt_put("[vectors] НЕ ВЫРОВНЕНА на 1 КБ - VECBASE переключать нельзя\n");
        for (;;) {
        }
    }

    // Проверить, что таблица действительно в памяти и линкер её заполнил.
    // Читаем слово из пула: там должен быть адрес C-обработчика. Если образ
    // не догрузился или смещение разошлось с vectors.S, увидим это здесь, а
    // не в виде молчащего сброса после переключения VECBASE.
    const uint32_t pool = ((const volatile uint32_t*)base)[HANDLER_POOL_OFFSET / 4];
    const uint32_t expected = (uint32_t)(uintptr_t)&basalt_panic_exc;

    basalt_put("[vectors] пул 0x");
    basalt_put_hex(pool, 8);
    basalt_put(", ожидался 0x");
    basalt_put_hex(expected, 8);
    basalt_put(pool == expected ? " - сходится\n" : " - РАСХОДИТСЯ\n");

    if (pool != expected) {
        basalt_put("[vectors] VECBASE не переключаю: таблица не та\n");
        for (;;) {
        }
    }

    // Переключить VECBASE. rsync обязателен: без него следующие инструкции
    // могут выполниться до того, как запись регистра вступит в силу.
    __asm__ volatile("wsr %0, vecbase\n\t"
                     "rsync" ::"r"(base)
                     : "memory");

    // Прочитать обратно: подтверждение, что записалось именно то, что хотели.
    uint32_t readback = 0;
    __asm__ volatile("rsr %0, vecbase" : "=r"(readback));

    basalt_put("[vectors] VECBASE 0x");
    basalt_put_hex(readback, 8);
    basalt_put(readback == base ? " совпадает\n" : " НЕ СОВПАДАЕТ\n");

    if (readback != base) {
        for (;;) {
        }
    }

    s_vectors_base = base;

    // Последняя строка функции: если её не видно, сбой случился внутри
    // инициализации, а не после неё.
    basalt_put("[vectors] готово\n");
}

uint32_t basalt_vectors_address(void)
{
    return s_vectors_base;
}
