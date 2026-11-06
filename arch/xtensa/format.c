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

// Запись справа налево: так не нужно заранее знать число цифр и не приходится
// разворачивать строку потом.
const char* basalt_format_u64(char* buf, size_t cap, uint64_t v)
{
    int i = (int)cap - 1;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    }
    while (v > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    return &buf[i];
}

void basalt_put_u64(uint64_t v)
{
    char buf[21];
    basalt_put(basalt_format_u64(buf, sizeof(buf), v));
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

// Округление к ближайшему, а не усечение. Разница видна сразу: 3 КБ из 346 -
// это 0.87%, и «0%» читалось бы как «ничего не занято», хотя занято. Половина
// прибавляется к числителю до деления - так формула остаётся целочисленной.
//
// Считается в 64 битах: part * 100 на 32-битном size_t переполнилось бы уже
// на 43 МБ, а в PSRAM их восемь.
const char* basalt_format_percent(char* buf, size_t cap, uint64_t part, uint64_t total)
{
    if (total == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }

    const uint64_t pct = (part * 100u + total / 2u) / total;
    return basalt_format_u64(buf, cap, pct);
}
