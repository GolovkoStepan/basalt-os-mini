// Тесты форматирования строки hex-дампа.
//
// Почему на маке: колонки и выравнивание проверяются сравнением строк, и это
// дешевле, чем ловить «таблица разъехалась на последней строке» глазами по
// экрану платы. Сюда же попадают случаи, которые на плате набирать жалко:
// непечатаемые байты, неполная строка, слишком маленький буфер.

#include <unity.h>

#include <string.h>

#include "hexdump.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static char s_line[HEXDUMP_LINE_MAX];

static void test_full_line(void)
{
    const uint8_t data[] = {'H', 'e', 'l', 'l', 'o', ',', ' ', 'w'};
    const size_t n = hexdump_line(s_line, sizeof(s_line), 0, data, sizeof(data));

    TEST_ASSERT_EQUAL_STRING(
        "00000000  48 65 6c 6c 6f 2c 20 77  |Hello, w|\n", s_line);
    TEST_ASSERT_EQUAL_size_t(strlen(s_line), n);
}

static void test_short_line_is_padded(void)
{
    const uint8_t data[] = {'a', 'b', 'c'};
    hexdump_line(s_line, sizeof(s_line), 0x10, data, sizeof(data));

    // Hex-колонка дополнена пробелами, поэтому '|' стоит на той же позиции,
    // что и в полной строке: иначе ASCII прыгает на последней строке файла.
    TEST_ASSERT_EQUAL_STRING(
        "00000010  61 62 63                 |abc|\n", s_line);
}

static void test_ascii_column_is_stable(void)
{
    // Сравниваем позицию '|' в полной и неполной строках: она обязана
    // совпасть, иначе таблица читается по диагонали.
    const uint8_t full[HEXDUMP_BYTES] = {0};
    const uint8_t one = 0;
    hexdump_line(s_line, sizeof(s_line), 0, full, sizeof(full));
    const char* bar_full = strchr(s_line, '|');

    char line2[HEXDUMP_LINE_MAX];
    hexdump_line(line2, sizeof(line2), 0, &one, 1);
    const char* bar_short = strchr(line2, '|');

    TEST_ASSERT_NOT_NULL(bar_full);
    TEST_ASSERT_NOT_NULL(bar_short);
    TEST_ASSERT_EQUAL_PTR(bar_full - s_line, bar_short - line2);
}

static void test_unprintable_becomes_dot(void)
{
    // 0x00, 0x1b (ESC), 0x7f (DEL) и 0xff - всё точки. ESC особенно важен:
    // попав в терминал как есть, он съел бы часть строки.
    const uint8_t data[] = {0x00, 0x1b, 0x7f, 0xff, 'A'};
    hexdump_line(s_line, sizeof(s_line), 0, data, sizeof(data));

    TEST_ASSERT_EQUAL_STRING(
        "00000000  00 1b 7f ff 41           |....A|\n", s_line);
}

static void test_printable_range_is_kept(void)
{
    const uint8_t data[] = {0x20, 0x7e};  // пробел и тильда - границы
    hexdump_line(s_line, sizeof(s_line), 0, data, sizeof(data));

    TEST_ASSERT_EQUAL_STRING(
        "00000000  20 7e                    | ~|\n", s_line);
}

static void test_offset_is_eight_digits(void)
{
    const uint8_t b = 0;
    hexdump_line(s_line, sizeof(s_line), 0x00abcdefu, &b, 1);

    TEST_ASSERT_EQUAL_STRING("00abcdef  00                       |.|\n", s_line);
}

static void test_empty_line_is_refused(void)
{
    // Ноль байт - это не строка дампа: вызывающий сам решает, что печатать.
    // Пустая «строка» с одной ASCII-рамкой только запутала бы.
    const uint8_t dummy = 0;
    TEST_ASSERT_EQUAL_size_t(0, hexdump_line(s_line, sizeof(s_line), 0, &dummy, 0));
}

static void test_small_buffer_is_refused(void)
{
    const uint8_t data[] = {1, 2};
    // Обрезанная строка дампа легко сходит за настоящие данные - лучше ноль.
    TEST_ASSERT_EQUAL_size_t(
        0, hexdump_line(s_line, HEXDUMP_LINE_MAX - 1, 0, data, sizeof(data)));
}

static void test_null_is_refused(void)
{
    const uint8_t data[] = {1};
    TEST_ASSERT_EQUAL_size_t(0, hexdump_line(NULL, 64, 0, data, sizeof(data)));
    TEST_ASSERT_EQUAL_size_t(0, hexdump_line(s_line, sizeof(s_line), 0, NULL, 1));
}

static void test_too_many_bytes_is_refused(void)
{
    const uint8_t data[HEXDUMP_BYTES + 1] = {0};
    TEST_ASSERT_EQUAL_size_t(
        0, hexdump_line(s_line, sizeof(s_line), 0, data, sizeof(data)));
}

static void test_line_fits_known_width(void)
{
    // Ширина строки - обещание, на которое рассчитывает экран 53x17.
    const uint8_t data[HEXDUMP_BYTES] = {0};
    const size_t n = hexdump_line(s_line, sizeof(s_line), 0, data, sizeof(data));

    TEST_ASSERT_EQUAL_size_t(46, n);  // 45 знаков + '\n'
    TEST_ASSERT_TRUE(n < HEXDUMP_LINE_MAX);
    TEST_ASSERT_TRUE(n <= 53);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_full_line);
    RUN_TEST(test_short_line_is_padded);
    RUN_TEST(test_ascii_column_is_stable);
    RUN_TEST(test_unprintable_becomes_dot);
    RUN_TEST(test_printable_range_is_kept);
    RUN_TEST(test_offset_is_eight_digits);
    RUN_TEST(test_empty_line_is_refused);
    RUN_TEST(test_small_buffer_is_refused);
    RUN_TEST(test_null_is_refused);
    RUN_TEST(test_too_many_bytes_is_refused);
    RUN_TEST(test_line_fits_known_width);

    return UNITY_END();
}
