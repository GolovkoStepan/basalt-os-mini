// Кодировщик UTF-8. Зачем отдельный модуль - см. utf8.h.

#include "utf8.h"

#include <stddef.h>

// Границы, которые надо проверять вручную, потому что формат их допускает:
// overlong-запись (тот же символ, записанный лишними байтами) - классический
// способ протащить NUL или '/' мимо фильтра.
#define CP_MAX          0x10FFFFu
#define SURROGATE_FIRST 0xD800u
#define SURROGATE_LAST  0xDFFFu

void utf8_reset(utf8_decoder* d)
{
    if (d == NULL) {
        return;
    }
    d->cp = 0;
    d->min = 0;
    d->need = 0;
}

utf8_status utf8_feed(utf8_decoder* d, uint8_t byte, uint32_t* out)
{
    if (d == NULL || out == NULL) {
        return UTF8_INVALID;
    }

    if (d->need == 0) {
        if (byte < 0x80) {
            *out = byte;
            return UTF8_DONE;
        }

        // Головка. 0xC0 и 0xC1 не годятся: ими начинаются только
        // overlong-последовательности, настоящих символов там нет.
        if (byte >= 0xC2 && byte <= 0xDF) {
            d->cp = byte & 0x1Fu;
            d->min = 0x80u;
            d->need = 1;
        } else if (byte >= 0xE0 && byte <= 0xEF) {
            d->cp = byte & 0x0Fu;
            d->min = 0x800u;
            d->need = 2;
        } else if (byte >= 0xF0 && byte <= 0xF4) {
            d->cp = byte & 0x07u;
            d->min = 0x10000u;
            d->need = 3;
        } else {
            return UTF8_INVALID;
        }
        return UTF8_PENDING;
    }

    // Байт-продолжение обязан иметь старшие биты 10, иначе это не хвост, а
    // что-то другое. Состояние при этом сбрасываем: продолжить уже нечего.
    if ((byte & 0xC0u) != 0x80u) {
        utf8_reset(d);
        return UTF8_INVALID;
    }

    d->cp = (d->cp << 6) | (byte & 0x3Fu);
    --d->need;

    if (d->need > 0) {
        return UTF8_PENDING;
    }

    const uint32_t cp = d->cp;
    const uint32_t min = d->min;
    utf8_reset(d);

    if (cp < min || cp > CP_MAX) {
        return UTF8_INVALID;
    }
    if (cp >= SURROGATE_FIRST && cp <= SURROGATE_LAST) {
        return UTF8_INVALID;
    }

    *out = cp;
    return UTF8_DONE;
}

size_t utf8_encode(uint32_t cp, char out[4])
{
    if (out == NULL) {
        return 0;
    }
    if (cp > CP_MAX) {
        return 0;
    }
    if (cp >= SURROGATE_FIRST && cp <= SURROGATE_LAST) {
        return 0;
    }

    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}
