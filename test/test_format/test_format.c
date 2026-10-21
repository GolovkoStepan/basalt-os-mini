// Тесты format.c на хосте. Проверяется текст, который ушёл бы в консоль.
//
// Почему именно эти случаи: вывод чисел участвует в отчёте о сбое, то есть
// работает тогда, когда уже всё сломалось, и повторить его на плате дорого.
// Крайние значения (ноль, максимум, младший разряд hex) - это ровно то, где
// самодельное форматирование обычно и ошибается.

#include <unity.h>

#include <stdint.h>
#include <string.h>

#include "console_stub.h"
#include "format.h"

void setUp(void)
{
    console_stub_reset();
}

void tearDown(void)
{
}

// --- десятичное ---

static void test_u64_zero(void)
{
    basalt_put_u64(0);
    TEST_ASSERT_EQUAL_STRING("0", console_stub_text());
}

static void test_u64_single_digits(void)
{
    basalt_put_u64(5);
    TEST_ASSERT_EQUAL_STRING("5", console_stub_text());

    console_stub_reset();
    basalt_put_u64(10);
    TEST_ASSERT_EQUAL_STRING("10", console_stub_text());
}

static void test_u64_boundaries(void)
{
    basalt_put_u64(9);
    TEST_ASSERT_EQUAL_STRING("9", console_stub_text());

    console_stub_reset();
    basalt_put_u64(99);
    TEST_ASSERT_EQUAL_STRING("99", console_stub_text());

    console_stub_reset();
    basalt_put_u64(100);
    TEST_ASSERT_EQUAL_STRING("100", console_stub_text());
}

// Без запятой в литерале: 18446744073709551615 не влезает в long long и
// компилятор ругается на константу, которую сам же и обрезал.
static void test_u64_max(void)
{
    basalt_put_u64(UINT64_MAX);
    TEST_ASSERT_EQUAL_STRING("18446744073709551615", console_stub_text());
}

static void test_u64_seconds_value(void)
{
    basalt_put_u64(1234567890u);
    TEST_ASSERT_EQUAL_STRING("1234567890", console_stub_text());
}

// --- hex ---

static void test_hex_full_word_padded(void)
{
    basalt_put_hex(0, 8);
    TEST_ASSERT_EQUAL_STRING("00000000", console_stub_text());

    console_stub_reset();
    basalt_put_hex(0xDEADBEEFu, 8);
    TEST_ASSERT_EQUAL_STRING("deadbeef", console_stub_text());
}

static void test_hex_byte(void)
{
    basalt_put_hex(0x0F, 2);
    TEST_ASSERT_EQUAL_STRING("0f", console_stub_text());
}

// Старшие разряды за пределами digits отбрасываются, а не печатаются:
// именно на это рассчитывает panic.c, печатая половину регистра.
static void test_hex_truncates_high_bits(void)
{
    basalt_put_hex(0x1234u, 2);
    TEST_ASSERT_EQUAL_STRING("34", console_stub_text());
}

static void test_hex_zero_digits_prints_nothing(void)
{
    basalt_put_hex(0xFFu, 0);
    TEST_ASSERT_EQUAL_STRING("", console_stub_text());
}

// --- строка ---

static void test_put_string(void)
{
    basalt_put("привет");
    TEST_ASSERT_EQUAL_STRING("привет", console_stub_text());
}

// Пустая строка не должна доходить до драйвера вообще: лишняя запись в FIFO
// на плате - это лишний шанс застрять в ожидании хоста.
static void test_put_empty_string_writes_nothing(void)
{
    basalt_put("");
    TEST_ASSERT_EQUAL_STRING("", console_stub_text());
}

// --- секунды ---

static void test_sec3_zero(void)
{
    basalt_put_sec3(0);
    TEST_ASSERT_EQUAL_STRING("0.000", console_stub_text());
}

static void test_sec3_borrows_from_nothing(void)
{
    basalt_put_sec3(999999u);
    TEST_ASSERT_EQUAL_STRING("0.999", console_stub_text());

    console_stub_reset();
    basalt_put_sec3(1000000u);
    TEST_ASSERT_EQUAL_STRING("1.000", console_stub_text());
}

static void test_sec3_milliseconds_are_truncated(void)
{
    // 3166000 мкс - это 3.166 с, а не 3.167: округления нет намеренно,
    // иначе счётчик выглядел бы прыгающим.
    basalt_put_sec3(3166000u);
    TEST_ASSERT_EQUAL_STRING("3.166", console_stub_text());
}

static void test_sec3_large_uptime(void)
{
    // 1234567890 мкс - это 1234 с и 567 мс: миллисекунды усекаются, а не
    // округляются.
    basalt_put_sec3(1234567890u);
    TEST_ASSERT_EQUAL_STRING("1234.567", console_stub_text());
}

// --- сочетание ---

// Так паникует panic.c: строка и число подряд, без разделителя.
static void test_sequence_matches_panic_line(void)
{
    basalt_put("EPC1     0x");
    basalt_put_hex(0x42001462u, 8);
    TEST_ASSERT_EQUAL_STRING("EPC1     0x42001462", console_stub_text());
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_u64_zero);
    RUN_TEST(test_u64_single_digits);
    RUN_TEST(test_u64_boundaries);
    RUN_TEST(test_u64_max);
    RUN_TEST(test_u64_seconds_value);
    RUN_TEST(test_hex_full_word_padded);
    RUN_TEST(test_hex_byte);
    RUN_TEST(test_hex_truncates_high_bits);
    RUN_TEST(test_hex_zero_digits_prints_nothing);
    RUN_TEST(test_put_string);
    RUN_TEST(test_put_empty_string_writes_nothing);
    RUN_TEST(test_sec3_zero);
    RUN_TEST(test_sec3_borrows_from_nothing);
    RUN_TEST(test_sec3_milliseconds_are_truncated);
    RUN_TEST(test_sec3_large_uptime);
    RUN_TEST(test_sequence_matches_panic_line);
    return UNITY_END();
}
