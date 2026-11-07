// Тесты разметки страницы `less` на маке.
//
// Почему здесь: страница - это арифметика над байтами (перенос длинной строки,
// табуляция, CRLF, разрезанный UTF-8), и ошибка в ней видна не как «не
// собралось», а как «на седьмой странице пропала строка». На плате такой
// сценарий пришлось бы набирать вручную, а здесь поток отдаётся массивом.
//
// Проверяются ровно те случаи, которые на глаз не отличить от правильных:
// строка длиной ровно в экран, последняя страница файла, конец файла на
// границе страницы.

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "pager.h"

void setUp(void)
{
}

void tearDown(void)
{
}

#define IN_MAX (8192u)

typedef struct {
    uint8_t in[IN_MAX];
    size_t len;
    size_t pos;  // позиция чтения; seek её двигает
    int fail_after;  // сколько чтений пройдёт успешно, -1 - все
    uint32_t reads;
} fake_stream;

static fake_stream s_stream;

static int fake_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* got)
{
    fake_stream* const f = ctx;
    *got = 0;

    if (f->fail_after >= 0 && (int)f->reads >= f->fail_after) {
        ++f->reads;
        return -1;
    }
    ++f->reads;

    const size_t left = f->len - f->pos;
    const size_t n = (left < cap) ? left : cap;
    if (n != 0) {
        memcpy(dst, f->in + f->pos, n);
        f->pos += n;
        *got = (uint32_t)n;
    }
    return 0;
}

static int fake_seek(void* ctx, uint32_t offset)
{
    fake_stream* const f = ctx;
    if (offset > f->len) {
        return -1;
    }
    f->pos = offset;
    return 0;
}

static pager s_pager;
static uint16_t s_cells[PAGER_ROWS][PAGER_COLS];

static void stream_set(const char* text)
{
    memset(&s_stream, 0, sizeof(s_stream));
    s_stream.fail_after = -1;
    s_stream.len = strlen(text);
    memcpy(s_stream.in, text, s_stream.len);

    pager_io io;
    io.ctx = &s_stream;
    io.read = fake_read;
    io.seek = fake_seek;
    pager_init(&s_pager, io);
}

// Строка экрана как C-строка: клетки до первого нуля.
static void row_text(int row, char* out)
{
    int n = 0;
    for (int c = 0; c < PAGER_COLS; ++c) {
        if (s_cells[row][c] == 0) {
            break;
        }
        out[n++] = (char)s_cells[row][c];
    }
    out[n] = '\0';
}

static void test_short_file_is_one_page(void)
{
    stream_set("one\ntwo\nthree\n");
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(3, p.rows);
    TEST_ASSERT_TRUE(p.eof);

    char line[PAGER_COLS + 1];
    row_text(0, line);
    TEST_ASSERT_EQUAL_STRING("one", line);
    row_text(2, line);
    TEST_ASSERT_EQUAL_STRING("three", line);
}

static void test_empty_file_has_no_rows(void)
{
    stream_set("");
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(0, p.rows);
    TEST_ASSERT_TRUE(p.eof);
}

// Файл, кончающийся переводом строки, не должен показывать лишнюю пустую
// строку: иначе у любого нормального файла под ним висела бы пустота, и
// «конец» нажимался бы дважды.
static void test_trailing_newline_is_not_a_row(void)
{
    stream_set("a\n");
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(1, p.rows);
    TEST_ASSERT_TRUE(p.eof);
}

// Пустые строки внутри файла - настоящие строки, их число важно.
static void test_empty_line_inside_is_kept(void)
{
    stream_set("a\n\nb\n");
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(3, p.rows);

    char line[PAGER_COLS + 1];
    row_text(1, line);
    TEST_ASSERT_EQUAL_STRING("", line);
    row_text(2, line);
    TEST_ASSERT_EQUAL_STRING("b", line);
}

static void test_crlf_is_not_shown(void)
{
    stream_set("hello\r\nworld\r\n");
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(2, p.rows);

    char line[PAGER_COLS + 1];
    row_text(0, line);
    TEST_ASSERT_EQUAL_STRING("hello", line);
    row_text(1, line);
    TEST_ASSERT_EQUAL_STRING("world", line);
}

static void test_long_line_wraps(void)
{
    // 60 символов в 53-клеточной строке: 53 на первой строке экрана, 7 на
    // второй. Разметка должна это показать, а не обрезать.
    char text[61];
    memset(text, 'x', 60);
    text[60] = '\0';

    stream_set(text);
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(2, p.rows);
    TEST_ASSERT_TRUE(p.eof);

    char line[PAGER_COLS + 1];
    row_text(0, line);
    TEST_ASSERT_EQUAL_size_t(PAGER_COLS, strlen(line));
    row_text(1, line);
    TEST_ASSERT_EQUAL_size_t(7, strlen(line));
}

// Строка ровно в ширину экрана переносится, но второй строки не занимает:
// иначе каждая строка такого файла давала бы пустую пару.
static void test_exact_width_line_does_not_grow(void)
{
    char text[PAGER_COLS + 2];
    memset(text, 'y', PAGER_COLS);
    text[PAGER_COLS] = '\n';
    text[PAGER_COLS + 1] = '\0';

    stream_set(text);
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(1, p.rows);
    TEST_ASSERT_TRUE(p.eof);
}

static void test_tab_fills_to_next_stop(void)
{
    stream_set("a\tb\n");
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(1, p.rows);

    char line[PAGER_COLS + 1];
    row_text(0, line);
    TEST_ASSERT_EQUAL_STRING("a       b", line);  // 'a' + 7 пробелов
}

static void test_tab_at_end_of_line_wraps(void)
{
    // Табуляция у самого края: остаток пробелов уходит на следующую строку,
    // а не пропадает.
    char text[PAGER_COLS + 3];
    memset(text, 'z', PAGER_COLS - 1);
    text[PAGER_COLS - 1] = '\t';
    text[PAGER_COLS] = 'q';
    text[PAGER_COLS + 1] = '\n';
    text[PAGER_COLS + 2] = '\0';

    stream_set(text);
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(2, p.rows);

    char line[PAGER_COLS + 1];
    row_text(1, line);
    // Табуляция с 52-й клетки идёт до 56-й: один пробел на первой строке,
    // остальные три - на второй, и только за ними 'q'.
    TEST_ASSERT_EQUAL_STRING("   q", line);
}

static void test_utf8_survives_page_boundary(void)
{
    // «Ж» - два байта. Строку длиной ровно в экран из однобайтовых символов
    // сдвигаем так, чтобы голова буквы попала на последнюю клетку.
    char text[PAGER_COLS * 2 + 8];
    size_t n = 0;
    for (int i = 0; i < PAGER_COLS - 1; ++i) {
        text[n++] = 'a';
    }
    text[n++] = (char)0xD0;  // голова «Ж»
    text[n++] = (char)0x96;  // продолжение
    text[n++] = '\n';
    text[n] = '\0';

    stream_set(text);
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(1, p.rows);

    // Буква целиком в последней клетке, а не разрезана: если бы разметка
    // резала по байтам, здесь оказались бы '?' и пустая клетка.
    TEST_ASSERT_EQUAL_UINT16(0x0416, s_cells[0][PAGER_COLS - 1]);
}

static void test_control_bytes_become_question(void)
{
    stream_set("a\x01\x1b" "b\n");
    const pager_page p = pager_fill(&s_pager, s_cells);

    char line[PAGER_COLS + 1];
    row_text(0, line);
    TEST_ASSERT_EQUAL_STRING("a??b", line);
}

static void test_second_page_continues_where_first_stopped(void)
{
    // 20 строк при 16 на странице: первая страница - 16 строк, вторая - 4.
    char text[400];
    size_t n = 0;
    for (int i = 0; i < 20; ++i) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "line%d\n", i);
    }

    stream_set(text);

    const pager_page first = pager_fill(&s_pager, s_cells);
    TEST_ASSERT_EQUAL_INT(PAGER_ROWS, first.rows);
    TEST_ASSERT_FALSE(first.eof);

    char line[PAGER_COLS + 1];
    row_text(0, line);
    TEST_ASSERT_EQUAL_STRING("line0", line);
    row_text(PAGER_ROWS - 1, line);
    TEST_ASSERT_EQUAL_STRING("line15", line);

    const pager_page second = pager_fill(&s_pager, s_cells);
    TEST_ASSERT_EQUAL_INT(4, second.rows);
    TEST_ASSERT_TRUE(second.eof);

    row_text(0, line);
    TEST_ASSERT_EQUAL_STRING("line16", line);
    row_text(3, line);
    TEST_ASSERT_EQUAL_STRING("line19", line);
}

// Смещение следующей страницы должно указывать в файл, а не в буфер: этим
// смещением страница перечитывается после возврата назад.
static void test_next_offset_is_absolute(void)
{
    char text[400];
    size_t n = 0;
    for (int i = 0; i < 20; ++i) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "line%d\n", i);
    }

    stream_set(text);
    const pager_page first = pager_fill(&s_pager, s_cells);

    // Смещение считается по файлу, а не по буферу: 10 строк "line0".."line9"
    // по 6 байт и 6 строк "line10".."line15" по 7.
    TEST_ASSERT_EQUAL_UINT32(10 * 6 + 6 * 7, first.next);
    TEST_ASSERT_EQUAL_CHAR('l', text[first.next]);
}

static void test_seek_replays_page(void)
{
    char text[400];
    size_t n = 0;
    for (int i = 0; i < 20; ++i) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "line%d\n", i);
    }

    stream_set(text);
    const pager_page first = pager_fill(&s_pager, s_cells);
    (void)pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(0, pager_seek(&s_pager, 0));
    const pager_page again = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(first.rows, again.rows);
    TEST_ASSERT_EQUAL_UINT32(first.next, again.next);

    char line[PAGER_COLS + 1];
    row_text(0, line);
    TEST_ASSERT_EQUAL_STRING("line0", line);
}

static void test_seek_without_stream_is_refused(void)
{
    stream_set("a\n");
    s_pager.io.seek = NULL;

    TEST_ASSERT_EQUAL_INT(-1, pager_seek(&s_pager, 0));
}

static void test_read_error_is_reported(void)
{
    stream_set("a\nb\n");
    s_stream.fail_after = 0;  // первое чтение уже отказ

    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(0, p.rows);
    TEST_ASSERT_TRUE(p.eof);
    TEST_ASSERT_EQUAL_INT(-1, s_pager.error);
}


// Файл ровно в 16 строк: страница полная и при этом последняя. Иначе «конец»
// пришлось бы нажимать второй раз на пустой странице, и это выглядело бы как
// потеря строк.
static void test_full_page_can_be_last(void)
{
    char text[400];
    size_t n = 0;
    for (int i = 0; i < PAGER_ROWS; ++i) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "r%d\n", i);
    }

    stream_set(text);
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_INT(PAGER_ROWS, p.rows);
    TEST_ASSERT_TRUE(p.eof);
}

// Длинная строка, которая не влезает в страницу: вторая страница обязана
// начаться с её середины, и ни один символ не должен пропасть.
static void test_long_line_spans_pages(void)
{
    // 20 строк по 53 символа: 16 строк на первой странице, остаток на второй.
    char text[64 * 24];
    size_t n = 0;
    for (int i = 0; i < 20; ++i) {
        for (int c = 0; c < PAGER_COLS; ++c) {
            text[n++] = (char)('a' + i);
        }
        text[n++] = '\n';
    }
    text[n] = '\0';

    stream_set(text);
    const pager_page first = pager_fill(&s_pager, s_cells);
    TEST_ASSERT_EQUAL_INT(PAGER_ROWS, first.rows);
    TEST_ASSERT_FALSE(first.eof);

    const pager_page second = pager_fill(&s_pager, s_cells);
    TEST_ASSERT_EQUAL_INT(4, second.rows);
    TEST_ASSERT_TRUE(second.eof);

    // Каждая строка начинается со своей буквы - значит, продолжений не
    // потерялось и лишних не появилось.
    TEST_ASSERT_EQUAL_UINT16('q', s_cells[0][0]);
    TEST_ASSERT_EQUAL_UINT16('t', s_cells[3][0]);
}

// Пустой файл: смещение следующей страницы - ноль, «листать» некуда.
static void test_empty_file_next_is_zero(void)
{
    stream_set("");
    const pager_page p = pager_fill(&s_pager, s_cells);

    TEST_ASSERT_EQUAL_UINT32(0, p.next);
    TEST_ASSERT_TRUE(p.eof);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_short_file_is_one_page);
    RUN_TEST(test_empty_file_has_no_rows);
    RUN_TEST(test_trailing_newline_is_not_a_row);
    RUN_TEST(test_empty_line_inside_is_kept);
    RUN_TEST(test_crlf_is_not_shown);
    RUN_TEST(test_long_line_wraps);
    RUN_TEST(test_exact_width_line_does_not_grow);
    RUN_TEST(test_tab_fills_to_next_stop);
    RUN_TEST(test_tab_at_end_of_line_wraps);
    RUN_TEST(test_utf8_survives_page_boundary);
    RUN_TEST(test_control_bytes_become_question);
    RUN_TEST(test_second_page_continues_where_first_stopped);
    RUN_TEST(test_next_offset_is_absolute);
    RUN_TEST(test_seek_replays_page);
    RUN_TEST(test_seek_without_stream_is_refused);
    RUN_TEST(test_read_error_is_reported);
    RUN_TEST(test_full_page_can_be_last);
    RUN_TEST(test_long_line_spans_pages);
    RUN_TEST(test_empty_file_next_is_zero);

    return UNITY_END();
}
