// Приёмник консоли для тестов tty.
//
// Зачем: в сборку каждого набора попадает весь build_src_filter, то есть и
// format.c, который нужен соседнему набору test_format. format.c линкуется с
// basalt_console_write, а её на хосте даёт заглушка того набора - сюда она не
// видна. Без этой функции набор не собирается, хотя ни один тест tty в
// консоль не пишет.
//
// Копия такая же, как в test/test_text/console_sink.c, и это осознанно:
// заглушка железа живёт в каталоге своего набора, как и
// test/test_format/console_stub.c.

#include <stddef.h>

#include "console_usb_jtag.h"

size_t basalt_console_write(const void* data, size_t len)
{
    (void)data;
    return len;
}
