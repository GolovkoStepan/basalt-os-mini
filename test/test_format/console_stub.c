// Заглушка консоли для хостовых тестов.
//
// format.c ничего не знает о том, куда пишет: он отдаёт байты в
// basalt_console_write. На хосте вместо USB Serial/JTAG - буфер в памяти,
// поэтому тест проверяет ровно тот текст, который ушёл бы в терминал.
// Тестировать отдельно «функцию преобразования» было бы слабее: настоящий
// код - вместе с консольным швом, и он же и проверяется.

#include <stddef.h>
#include <string.h>

#include "console_usb_jtag.h"
#include "console_stub.h"

static char s_buffer[512];
static size_t s_used;

void console_stub_reset(void)
{
    s_used = 0;
    s_buffer[0] = '\0';
}

const char* console_stub_text(void)
{
    s_buffer[s_used] = '\0';
    return s_buffer;
}

size_t basalt_console_write(const void* data, size_t len)
{
    const size_t room = sizeof(s_buffer) - 1 - s_used;
    const size_t n = len < room ? len : room;
    memcpy(&s_buffer[s_used], data, n);
    s_used += n;
    s_buffer[s_used] = '\0';
    return n;
}
