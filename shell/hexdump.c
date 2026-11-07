// Форматирование строки hex-дампа. См. hexdump.h - там про ширину экрана.

#include "hexdump.h"

static const char HEX_DIGITS[] = "0123456789abcdef";

// Печатаемый ASCII - от пробела до тильды. Остальное точкой: управляющие
// символы в терминале не видны, а перевод строки в середине строки дампа
// сломал бы таблицу.
static char ascii_of(uint8_t b)
{
    return (b >= 0x20 && b < 0x7f) ? (char)b : '.';
}

size_t hexdump_line(char* out, size_t cap, uint32_t offset,
                    const uint8_t* data, size_t len)
{
    if (out == NULL || cap < HEXDUMP_LINE_MAX || data == NULL ||
        len == 0 || len > HEXDUMP_BYTES) {
        return 0;
    }

    size_t pos = 0;

    // Смещение восемью знаками: адрес в пределах 16 МБ - это шесть знаков, но
    // ровно восемь колонка остаётся постоянной на всём диапазоне flash и на
    // файлах любого размера, которые сюда попадут.
    for (int shift = 28; shift >= 0; shift -= 4) {
        out[pos++] = HEX_DIGITS[(offset >> shift) & 0xf];
    }
    out[pos++] = ' ';
    out[pos++] = ' ';

    for (size_t i = 0; i < HEXDUMP_BYTES; ++i) {
        if (i < len) {
            out[pos++] = HEX_DIGITS[data[i] >> 4];
            out[pos++] = HEX_DIGITS[data[i] & 0xf];
        } else {
            // Пробелы вместо hex: без них ASCII-колонка съезжает на последней
            // строке, и глаз цепляется за это раньше, чем за содержимое.
            out[pos++] = ' ';
            out[pos++] = ' ';
        }
        out[pos++] = ' ';
    }

    out[pos++] = ' ';
    out[pos++] = '|';

    for (size_t i = 0; i < len; ++i) {
        out[pos++] = ascii_of(data[i]);
    }
    out[pos++] = '|';
    out[pos++] = '\n';
    out[pos] = '\0';

    return pos;
}
