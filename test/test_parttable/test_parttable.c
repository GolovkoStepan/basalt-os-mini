// Тесты разбора таблицы разделов на хосте.
//
// Почему здесь: ошибка в смещении поля даёт не отказ, а раздел с мусорным
// адресом - и выясняется это записью в чужое место flash. На маке такая
// проверка стоит миллисекунды, на плате - испорченный образ.

#include <unity.h>

#include <string.h>

#include "parttable.h"

void setUp(void)
{
}

void tearDown(void)
{
}

// Одна запись в буфере теста. Поля раскладываются побайтно: так же, как они
// лежат в flash, и без зависимости от выравнивания структуры в компиляторе.
static void put_entry(uint8_t* table, unsigned index, uint16_t magic, uint8_t type,
                      uint8_t subtype, uint32_t address, uint32_t size, const char* label)
{
    uint8_t* e = table + (size_t)index * BASALT_PART_ENTRY_SIZE;
    memset(e, 0, BASALT_PART_ENTRY_SIZE);

    e[0] = (uint8_t)(magic & 0xFFu);
    e[1] = (uint8_t)(magic >> 8);
    e[2] = type;
    e[3] = subtype;
    e[4] = (uint8_t)(address & 0xFFu);
    e[5] = (uint8_t)((address >> 8) & 0xFFu);
    e[6] = (uint8_t)((address >> 16) & 0xFFu);
    e[7] = (uint8_t)((address >> 24) & 0xFFu);
    e[8] = (uint8_t)(size & 0xFFu);
    e[9] = (uint8_t)((size >> 8) & 0xFFu);
    e[10] = (uint8_t)((size >> 16) & 0xFFu);
    e[11] = (uint8_t)((size >> 24) & 0xFFu);
    memcpy(e + 12, label, strlen(label));
}

static uint8_t s_table[5 * BASALT_PART_ENTRY_SIZE];

static void fill_ff(uint8_t* p, size_t len)
{
    memset(p, 0xFF, len);
}

// ---------------------------------------------------------------------------

void test_reads_first_entry(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 0, 0, 0x10000, 0x200000, "factory");

    basalt_partition_t p;
    TEST_ASSERT_EQUAL_INT(BASALT_PART_OK,
                          basalt_parttable_get(s_table, sizeof(s_table), 0, &p));
    TEST_ASSERT_EQUAL_UINT32(0x10000, p.address);
    TEST_ASSERT_EQUAL_UINT32(0x200000, p.size);
    TEST_ASSERT_EQUAL_UINT8(0, p.type);
    TEST_ASSERT_EQUAL_UINT8(0, p.subtype);
    TEST_ASSERT_EQUAL_STRING("factory", p.label);
}

void test_own_subtype_is_kept_as_number(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 1, 0x40, 0x210000, 0xDF0000, "storage");

    basalt_partition_t p;
    TEST_ASSERT_EQUAL_INT(BASALT_PART_OK,
                          basalt_parttable_get(s_table, sizeof(s_table), 0, &p));
    TEST_ASSERT_EQUAL_UINT8(0x40, p.subtype);
    TEST_ASSERT_EQUAL_UINT32(0x210000, p.address);
    TEST_ASSERT_EQUAL_UINT32(0xDF0000, p.size);
}

// Стирание, за которым записи нет: таблица кончилась, но не контрольной суммой.
void test_erased_entry_ends_table(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 0, 0, 0x10000, 0x100000, "factory");

    basalt_partition_t p;
    TEST_ASSERT_EQUAL_INT(BASALT_PART_END_FF,
                          basalt_parttable_get(s_table, sizeof(s_table), 1, &p));
}

void test_md5_entry_ends_table(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 0, 0, 0x10000, 0x100000, "factory");
    put_entry(s_table, 1, BASALT_PART_MAGIC_MD5, 0, 0, 0, 0, "");

    basalt_partition_t p;
    TEST_ASSERT_EQUAL_INT(BASALT_PART_END_MD5,
                          basalt_parttable_get(s_table, sizeof(s_table), 1, &p));
}

void test_foreign_magic_is_not_table(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, 0x1234, 0, 0, 0x10000, 0x100000, "factory");

    basalt_partition_t p;
    TEST_ASSERT_EQUAL_INT(BASALT_PART_BAD_MAGIC,
                          basalt_parttable_get(s_table, sizeof(s_table), 0, &p));
}

// Метка без нуля внутри 16 байт: так выглядит имя ровно из 16 символов.
void test_label_without_terminator_is_copyable(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 1, 0x40, 0x210000, 0x1000, "0123456789abcdef");

    basalt_partition_t p;
    TEST_ASSERT_EQUAL_INT(BASALT_PART_OK,
                          basalt_parttable_get(s_table, sizeof(s_table), 0, &p));
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", p.label);
}

// Запись за пределами прочитанного: вызывающий прочитал меньше, чем записей.
void test_entry_past_buffer_ends_table(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 0, 0, 0x10000, 0x100000, "factory");

    basalt_partition_t p;
    TEST_ASSERT_EQUAL_INT(BASALT_PART_END_FF,
                          basalt_parttable_get(s_table, BASALT_PART_ENTRY_SIZE, 1, &p));
}

void test_short_buffer_is_bad_arg(void)
{
    basalt_partition_t p;
    TEST_ASSERT_EQUAL_INT(BASALT_PART_BAD_ARG,
                          basalt_parttable_get(s_table, BASALT_PART_ENTRY_SIZE - 1, 0, &p));
}

void test_null_arguments_are_bad_arg(void)
{
    basalt_partition_t p;
    TEST_ASSERT_EQUAL_INT(BASALT_PART_BAD_ARG,
                          basalt_parttable_get(NULL, sizeof(s_table), 0, &p));
    TEST_ASSERT_EQUAL_INT(BASALT_PART_BAD_ARG,
                          basalt_parttable_get(s_table, sizeof(s_table), 0, NULL));
}

// ---------------------------------------------------------------------------

void test_find_by_label(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 1, 2, 0x9000, 0x6000, "nvs");
    put_entry(s_table, 1, BASALT_PART_MAGIC, 0, 0, 0x10000, 0x200000, "factory");
    put_entry(s_table, 2, BASALT_PART_MAGIC, 1, 0x40, 0x210000, 0xDF0000, "storage");
    put_entry(s_table, 3, BASALT_PART_MAGIC_MD5, 0, 0, 0, 0, "");

    basalt_partition_t p;
    TEST_ASSERT_TRUE(basalt_parttable_find(s_table, sizeof(s_table), "storage", &p));
    TEST_ASSERT_EQUAL_UINT32(0x210000, p.address);
    TEST_ASSERT_EQUAL_UINT32(0xDF0000, p.size);
    TEST_ASSERT_EQUAL_UINT8(0x40, p.subtype);
}

void test_find_stops_at_md5(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 0, 0, 0x10000, 0x100000, "factory");
    put_entry(s_table, 1, BASALT_PART_MAGIC_MD5, 0, 0, 0, 0, "");
    // Мусор после контрольной суммы: искать за ней нельзя, это не таблица.
    put_entry(s_table, 2, BASALT_PART_MAGIC, 1, 0x40, 0x210000, 0x1000, "storage");

    basalt_partition_t p;
    TEST_ASSERT_FALSE(basalt_parttable_find(s_table, sizeof(s_table), "storage", &p));
}

void test_find_missing_returns_false(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 0, 0, 0x10000, 0x100000, "factory");

    basalt_partition_t p;
    TEST_ASSERT_FALSE(basalt_parttable_find(s_table, sizeof(s_table), "storage", &p));
}

// Поиск не должен портить структуру при промахе: вызывающий вправе передать
// один и тот же out в цикле по нескольким именам.
void test_find_does_not_touch_out_on_miss(void)
{
    fill_ff(s_table, sizeof(s_table));
    put_entry(s_table, 0, BASALT_PART_MAGIC, 0, 0, 0x10000, 0x100000, "factory");

    basalt_partition_t p;
    memset(&p, 0xAA, sizeof(p));
    TEST_ASSERT_FALSE(basalt_parttable_find(s_table, sizeof(s_table), "storage", &p));
    TEST_ASSERT_EQUAL_UINT32(0xAAAAAAAAu, p.address);
}

void test_find_null_label_returns_false(void)
{
    basalt_partition_t p;
    TEST_ASSERT_FALSE(basalt_parttable_find(s_table, sizeof(s_table), NULL, &p));
}

// ---------------------------------------------------------------------------

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_reads_first_entry);
    RUN_TEST(test_own_subtype_is_kept_as_number);
    RUN_TEST(test_erased_entry_ends_table);
    RUN_TEST(test_md5_entry_ends_table);
    RUN_TEST(test_foreign_magic_is_not_table);
    RUN_TEST(test_label_without_terminator_is_copyable);
    RUN_TEST(test_entry_past_buffer_ends_table);
    RUN_TEST(test_short_buffer_is_bad_arg);
    RUN_TEST(test_null_arguments_are_bad_arg);

    RUN_TEST(test_find_by_label);
    RUN_TEST(test_find_stops_at_md5);
    RUN_TEST(test_find_missing_returns_false);
    RUN_TEST(test_find_does_not_touch_out_on_miss);
    RUN_TEST(test_find_null_label_returns_false);

    return UNITY_END();
}
