// Приёмник консоли для тестов разбора таблицы разделов.
//
// Зачем: в сборку набора попадает весь build_src_filter, то есть и format.c,
// которому нужна basalt_console_write. На хосте её даёт заглушка своего
// набора - отсюда эта копия. Так же сделано в test/test_tty и соседних.
//
// Сам модуль таблицы консоль не трогает: он только разбирает байты. Заглушка
// здесь потому, что набор собирается целиком, а не по файлу.

#include <stddef.h>

#include "console_usb_jtag.h"

size_t basalt_console_write(const void* data, size_t len)
{
    (void)data;
    return len;
}
