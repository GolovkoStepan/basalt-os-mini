// Длительности: разбор и печать. Устройство и причины - в duration.h.

#include "duration.h"

#include <string.h>

// Единицы разбираются не таблицей, а явными ветками: суффикс "ms" и суффикс
// "m" различаются вторым символом, и таблица с одним символом на единицу эту
// разницу потеряла бы («500ms» прочиталось бы как 500 минут).
static bool parse_component(const char** p, uint32_t* out_ms)
{
    const char* s = *p;
    if (*s < '0' || *s > '9') {
        return false;
    }

    uint32_t value = 0;
    while (*s >= '0' && *s <= '9') {
        // Больше 24 часов всё равно откажем, поэтому переполнения боимся
        // заранее: число из десяти цифр уже не длительность.
        if (value > 1000000u) {
            return false;
        }
        value = value * 10u + (uint32_t)(*s - '0');
        ++s;
    }

    uint32_t unit = 1000u;  // без суффикса - секунды, как у sleep(1)
    if (s[0] == 'm' && s[1] == 's') {
        unit = 1u;
        s += 2;
    } else if (*s == 'h') {
        unit = 3600u * 1000u;
        ++s;
    } else if (*s == 'm') {
        unit = 60u * 1000u;
        ++s;
    } else if (*s == 's') {
        unit = 1000u;
        ++s;
    } else if (*s != '\0' && (*s < '0' || *s > '9')) {
        // Неизвестная буква - не длительность. Проверка нужна, чтобы "5x"
        // не превратилось в 5 секунд с молча выброшенным суффиксом.
        return false;
    }

    if (value > DURATION_MAX_MS / unit) {
        return false;
    }
    *out_ms = value * unit;
    *p = s;
    return true;
}

bool duration_parse(const char* text, uint32_t* out_ms)
{
    if (text == NULL || out_ms == NULL || text[0] == '\0') {
        return false;
    }

    uint32_t total = 0;
    const char* p = text;
    while (*p != '\0') {
        uint32_t part = 0;
        if (!parse_component(&p, &part)) {
            return false;
        }
        if (part > DURATION_MAX_MS - total) {
            return false;
        }
        total += part;
    }

    if (total == 0) {
        return false;
    }
    *out_ms = total;
    return true;
}

// ---------------------------------------------------------------------------
// Печать
// ---------------------------------------------------------------------------

static size_t app(char* buf, size_t cap, size_t at, const char* s)
{
    while (*s != '\0' && at + 1 < cap) {
        buf[at++] = *s++;
    }
    if (cap != 0) {
        buf[(at < cap) ? at : cap - 1] = '\0';
    }
    return at;
}

static size_t app_u32(char* buf, size_t cap, size_t at, uint32_t v)
{
    char num[11];
    int n = 0;
    if (v == 0) {
        num[n++] = '0';
    }
    while (v != 0) {
        num[n++] = (char)('0' + v % 10u);
        v /= 10u;
    }
    for (int i = n - 1; i >= 0; --i) {
        if (at + 1 < cap) {
            buf[at++] = num[i];
        }
    }
    if (cap != 0) {
        buf[(at < cap) ? at : cap - 1] = '\0';
    }
    return at;
}

const char* duration_text(char* buf, size_t cap, uint32_t ms)
{
    if (buf == NULL || cap == 0) {
        return buf;
    }

    char* const out = buf;
    if (ms < 1000u) {
        size_t at = app_u32(buf, cap, 0, ms);
        app(buf, cap, at, " мс");
        return out;
    }

    const uint32_t hours = ms / 3600000u;
    const uint32_t mins = (ms % 3600000u) / 60000u;
    const uint32_t secs = (ms % 60000u) / 1000u;

    size_t at = 0;
    if (hours != 0) {
        at = app_u32(buf, cap, at, hours);
        at = app(buf, cap, at, " ч ");
    }
    if (mins != 0) {
        at = app_u32(buf, cap, at, mins);
        at = app(buf, cap, at, " мин ");
    }
    // Секунды печатаются, если они есть или если выше ничего не было: «1 мин»
    // без «0 с» читается лучше, а «0 с» вместо пустой строки нужен.
    if (secs != 0 || (hours == 0 && mins == 0)) {
        at = app_u32(buf, cap, at, secs);
        at = app(buf, cap, at, " с");
    } else if (at >= 1 && buf[at - 1] == ' ') {
        // Убрать хвостовой пробел: «1 мин » с пробелом на конце выглядит
        // как обрезанная строка.
        buf[at - 1] = '\0';
        --at;
    }
    (void)at;
    return out;
}
