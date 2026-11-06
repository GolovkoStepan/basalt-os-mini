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

// --- проценты ---

// Возвращает строку в переданном буфере, а не печатает: так же устроен
// basalt_format_u64, и вызывающий сам решает, куда её девать.
static const char* pct(uint64_t part, uint64_t total)
{
    static char buf[8];
    return basalt_format_percent(buf, sizeof(buf), part, total);
}

// Деления на ноль быть не должно: у пустой области процента нет, но напечатать
// что-то надо. Ноль - самое честное из чисел.
static void test_percent_zero_total(void)
{
    TEST_ASSERT_EQUAL_STRING("0", pct(0, 0));
    TEST_ASSERT_EQUAL_STRING("0", pct(123, 0));
}

// Округление к ближайшему: 3 КБ из 346 - это 0.87%, и «0%» читалось бы как
// «память не занята». Именно это округление и есть причина завести функцию.
static void test_percent_rounds_not_truncates(void)
{
    TEST_ASSERT_EQUAL_STRING("1", pct(3, 346));
    TEST_ASSERT_EQUAL_STRING("1", pct(5, 1000));
}

// Границы: пусто, всё, почти всё.
static void test_percent_edges(void)
{
    TEST_ASSERT_EQUAL_STRING("0", pct(0, 100));
    TEST_ASSERT_EQUAL_STRING("100", pct(100, 100));
    TEST_ASSERT_EQUAL_STRING("50", pct(50, 100));
    TEST_ASSERT_EQUAL_STRING("100", pct(8192, 8192));
}

// 0.5 и больше округляется вверх, меньше - вниз: половина прибавляется до
// деления, поэтому ровно 0.5 идёт вверх.
static void test_percent_halves(void)
{
    TEST_ASSERT_EQUAL_STRING("50", pct(1, 2));
    TEST_ASSERT_EQUAL_STRING("33", pct(1, 3));
    TEST_ASSERT_EQUAL_STRING("67", pct(2, 3));
    TEST_ASSERT_EQUAL_STRING("1", pct(1, 200));
}

// Настоящие числа с платы: стек 992 из 8192 и куча PSRAM 8189 из 8192.
static void test_percent_board_values(void)
{
    TEST_ASSERT_EQUAL_STRING("12", pct(992, 8192));
    TEST_ASSERT_EQUAL_STRING("99", pct(343, 346));
    TEST_ASSERT_EQUAL_STRING("100", pct(8189, 8192));
}

// part * 100 переполнило бы 32-битный size_t уже на 43 МБ, а в PSRAM их восемь.
// Проверка держит формулу в 64 битах при значениях, которые реально бывают.
static void test_percent_large_values(void)
{
    TEST_ASSERT_EQUAL_STRING("50", pct(1u << 22, 1u << 23));      // 4 МБ из 8 МБ
    TEST_ASSERT_EQUAL_STRING("100", pct(1u << 23, 1u << 23));
    // 1.56% - округление вверх до 2, а не усечение до 1.
    TEST_ASSERT_EQUAL_STRING("2", pct(1u << 17, 1u << 23));
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
    RUN_TEST(test_percent_zero_total);
    RUN_TEST(test_percent_rounds_not_truncates);
    RUN_TEST(test_percent_edges);
    RUN_TEST(test_percent_halves);
    RUN_TEST(test_percent_board_values);
    RUN_TEST(test_percent_large_values);
    return UNITY_END();
}
