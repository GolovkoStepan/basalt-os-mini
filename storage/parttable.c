// Разбор таблицы разделов flash. См. parttable.h.

#include "parttable.h"

#include <string.h>

// Поля записи читаются побайтно: таблица в flash лежит в порядке little-endian,
// а выравнивания у буфера-источника может не быть (он приходит из malloc или
// со стека). Приведение к uint32_t* на xtensa сработало бы через раз - а это
// как раз тот класс ошибок, который ищут вечером на плате.
static uint16_t rd16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}

static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

basalt_part_status_t basalt_parttable_get(const uint8_t* table, size_t length, unsigned index,
                                          basalt_partition_t* out)
{
    if (table == NULL || out == NULL || length < BASALT_PART_ENTRY_SIZE) {
        return BASALT_PART_BAD_ARG;
    }

    // Умножение в size_t: index приходит из вызывающего кода, и на большом
    // значении uint32_t переполнился бы раньше сравнения с length.
    const size_t offset = (size_t)index * BASALT_PART_ENTRY_SIZE;
    if (offset + BASALT_PART_ENTRY_SIZE > length) {
        // Дальше буфера записей нет. Это не ошибка разбора: так выглядит
        // «таблица кончилась», если в буфер прочитано меньше, чем записей.
        return BASALT_PART_END_FF;
    }

    const uint8_t* const e = table + offset;
    const uint16_t magic = rd16(e);

    if (magic == BASALT_PART_MAGIC_MD5) {
        return BASALT_PART_END_MD5;
    }
    // 0xFF - стёртая flash. Проверяем два байта, а не весь magic: так же
    // поступает сам формат, и запись из нулей сюда не попадёт.
    if (e[0] == 0xFFu && e[1] == 0xFFu) {
        return BASALT_PART_END_FF;
    }
    if (magic != BASALT_PART_MAGIC) {
        return BASALT_PART_BAD_MAGIC;
    }

    out->type = e[2];
    out->subtype = e[3];
    out->address = rd32(e + 4);
    out->size = rd32(e + 8);

    // Метка в записи не обязана иметь ноль-терминатор: если имя занимает все
    // 16 байт, нуля там нет. Поэтому копируем ровно 16 и ставим свой ноль.
    memcpy(out->label, e + 12, BASALT_PART_LABEL_MAX - 1);
    out->label[BASALT_PART_LABEL_MAX - 1] = '\0';

    return BASALT_PART_OK;
}

bool basalt_parttable_find(const uint8_t* table, size_t length, const char* label,
                           basalt_partition_t* out)
{
    if (label == NULL) {
        return false;
    }

    // Граница перебора - по длине буфера, а не по числу из формата: так
    // вызывающий сам решает, сколько прочитал, и лишний basalt_flash_read не
    // нужен.
    const unsigned count = (unsigned)(length / BASALT_PART_ENTRY_SIZE);
    for (unsigned i = 0; i < count; ++i) {
        basalt_partition_t entry;
        const basalt_part_status_t status = basalt_parttable_get(table, length, i, &entry);
        if (status != BASALT_PART_OK) {
            // Любой конец таблицы (MD5 или стёртая flash) и любой мусор
            // означают одно: дальше искать нечего.
            return false;
        }
        if (strcmp(entry.label, label) == 0) {
            *out = entry;
            return true;
        }
    }
    return false;
}
