// Вывод чисел в консоль без printf. См. format.h - там про «почему свой».

#include "format.h"

#include <stddef.h>

#include "console_usb_jtag.h"

void basalt_put(const char* s)
{
    const char* p = s;
    while (*p) {
        ++p;
    }
    if (p != s) {
        basalt_console_write(s, (size_t)(p - s));
    }
}

void basalt_put_u64(uint64_t v)
{
    char buf[21];
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    }
    while (v > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    basalt_put(&buf[i]);
}

void basalt_put_hex(uint32_t v, int digits)
{
    static const char kDigits[] = "0123456789abcdef";
    char buf[9];
    for (int i = digits - 1; i >= 0; --i) {
        buf[i] = kDigits[v & 0xF];
        v >>= 4;
    }
    buf[digits] = '\0';
    basalt_put(buf);
}

void basalt_put_sec3(uint64_t us)
{
    basalt_put_u64(us / 1000000u);
    basalt_put(".");

    const uint32_t ms = (uint32_t)((us % 1000000u) / 1000u);
    char buf[4];
    buf[0] = (char)('0' + (ms / 100) % 10);
    buf[1] = (char)('0' + (ms / 10) % 10);
    buf[2] = (char)('0' + ms % 10);
    buf[3] = '\0';
    basalt_put(buf);
}
