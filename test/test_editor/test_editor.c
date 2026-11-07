// Тесты ядра редактора на маке.
//
// Почему здесь: в редакторе всё, что стоит вечера на плате, - это арифметика
// над байтами. «Стерлось пол-буквы», «стрелка вниз уехала влево», «конец
// файла не показался» - на 53-колоночном экране каждый такой случай пришлось
// бы ловить глазами, а здесь текст и клавиши сравниваются напрямую.
//
// Проверяются границы: начало и конец буфера, склейка строк, табуляция на
// границе экрана, кириллица, полная ёмкость.

#include <unity.h>

#include <string.h>

#include "editor.h"

void setUp(void)
{
}

void tearDown(void)
{
}

#define CAP 4096u

static char s_buf[CAP];
static editor s_ed;

static void load(const char* text)
{
    const size_t n = strlen(text);
    memcpy(s_buf, text, n);
    ed_init(&s_ed, s_buf, CAP);
    ed_load(&s_ed, (uint32_t)n);
}

// Подать строку по байтам, как их прислал бы tty.
static void type(const char* text)
{
    for (const char* p = text; *p != '\0'; ++p) {
        ed_input(&s_ed, (uint8_t)*p);
    }
}

// Escape-последовательность ESC [ <хвост>: "A" - стрелка, "3~" - Delete.
static ed_event seq(const char* tail)
{
    ed_input(&s_ed, 0x1B);
    ed_input(&s_ed, '[');

    ed_event ev = ED_NONE;
    for (const char* p = tail; *p != '\0'; ++p) {
        ev = ed_input(&s_ed, (uint8_t)*p);
    }
    return ev;
}

static void text_is(const char* expected)
{
    const uint32_t n = (uint32_t)strlen(expected);
    TEST_ASSERT_EQUAL_UINT32(n, s_ed.len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(expected, ed_data(&s_ed), n));
}

// --- загрузка ---

static void test_load_empty_file(void)
{
    load("");
    TEST_ASSERT_EQUAL_UINT32(0, ed_len(&s_ed));
    TEST_ASSERT_EQUAL_UINT32(0, s_ed.cursor);
    TEST_ASSERT_FALSE(ed_dirty(&s_ed));
}

static void test_load_crlf_becomes_lf(void)
{
    load("a\r\nb\r\n");
    text_is("a\nb\n");
}

// CR внутри строки (не перед LF) - не CRLF: его никто не обещал выбросить,
// и потеря значила бы молча изменённый файл.
static void test_lone_cr_is_kept(void)
{
    load("a\rb");
    text_is("a\rb");
}

static void test_load_strips_bom(void)
{
    load("\xEF\xBB\xBFx");
    text_is("x");
}

// Открыть файл и сразу выйти - не правка: иначе первый же Ctrl-Q просил бы
// подтверждения на нетронутом файле.
static void test_load_clears_dirty(void)
{
    load("abc");
    type("d");
    TEST_ASSERT_TRUE(ed_dirty(&s_ed));

    load("abc");
    TEST_ASSERT_FALSE(ed_dirty(&s_ed));
}

// --- набор ---

static void test_type_ascii(void)
{
    load("");
    type("hi");
    text_is("hi");
    TEST_ASSERT_EQUAL_UINT32(2, s_ed.cursor);
    TEST_ASSERT_TRUE(ed_dirty(&s_ed));
}

static void test_backspace_removes_previous(void)
{
    load("abc");
    seq("F");  // в конец строки: Backspace стирает перед курсором
    ed_input(&s_ed, 0x7F);
    text_is("ab");
    TEST_ASSERT_EQUAL_UINT32(2, s_ed.cursor);
}

static void test_backspace_at_start_is_bell(void)
{
    load("abc");
    s_ed.cursor = 0;
    TEST_ASSERT_EQUAL_INT(ED_BELL, ed_input(&s_ed, 0x7F));
    text_is("abc");
}

static void test_enter_splits_line(void)
{
    load("ab");
    seq("H");
    TEST_ASSERT_EQUAL_UINT32(0, s_ed.cursor);

    ed_input(&s_ed, '\r');
    text_is("\nab");
}

// Backspace в начале строки склеивает её с предыдущей - это и есть «стереть
// перевод строки», другого способа у него нет.
static void test_backspace_joins_lines(void)
{
    load("a\nb");
    seq("B");  // на вторую строку, в её начало
    TEST_ASSERT_EQUAL_UINT32(2, s_ed.cursor);

    ed_input(&s_ed, 0x7F);
    text_is("ab");
}

static void test_delete_joins_lines(void)
{
    load("a\nb");
    seq("F");    // в конец первой строки, перед переводом
    seq("3~");   // Delete стирает сам перевод
    text_is("ab");
}

static void test_delete_at_end_is_bell(void)
{
    load("ab");
    seq("F");
    TEST_ASSERT_EQUAL_INT(ED_BELL, seq("3~"));
    text_is("ab");
}

// --- движение ---

static void test_home_and_end(void)
{
    load("abc\ndef");
    seq("F");
    TEST_ASSERT_EQUAL_UINT32(3, s_ed.cursor);
    seq("H");
    TEST_ASSERT_EQUAL_UINT32(0, s_ed.cursor);
}

static void test_left_at_line_start_goes_to_previous_end(void)
{
    load("ab\ncd");
    seq("B");  // вниз, на вторую строку
    TEST_ASSERT_EQUAL_UINT32(3, s_ed.cursor);

    seq("D");  // влево с начала строки
    TEST_ASSERT_EQUAL_UINT32(2, s_ed.cursor);
}

// Стрелки вверх-вниз держат колонку: проход через короткую строку не должен
// оставлять курсор сдвинутым влево.
static void test_up_down_keep_goal_column(void)
{
    load("abcdef\nxy\nabcdef");

    seq("B");
    seq("B");
    seq("F");
    TEST_ASSERT_EQUAL_UINT32(16, s_ed.cursor);

    seq("A");
    uint32_t line = 0;
    uint32_t col = 0;
    ed_line_col(&s_ed, &line, &col);
    TEST_ASSERT_EQUAL_UINT32(2, line);
    TEST_ASSERT_EQUAL_UINT32(3, col);  // строка короче - курсор в её конец

    seq("A");
    ed_line_col(&s_ed, &line, &col);
    TEST_ASSERT_EQUAL_UINT32(1, line);
    TEST_ASSERT_EQUAL_UINT32(7, col);  // прежняя колонка восстановлена
}

static void test_up_at_first_line_does_not_move(void)
{
    load("abc");
    TEST_ASSERT_EQUAL_INT(ED_NONE, seq("A"));
    TEST_ASSERT_EQUAL_UINT32(0, s_ed.cursor);
}

static void test_right_at_end_does_not_move(void)
{
    load("ab");
    seq("F");
    TEST_ASSERT_EQUAL_INT(ED_NONE, seq("C"));
    TEST_ASSERT_EQUAL_UINT32(2, s_ed.cursor);
}

// --- табуляция и горизонтальная прокрутка ---

static void test_tab_expands_to_cells(void)
{
    load("\tx");
    uint16_t cells[EDITOR_COLS];
    const int n = ed_row(&s_ed, 0, cells, EDITOR_COLS);

    TEST_ASSERT_EQUAL_INT(9, n);
    for (int i = 0; i < 8; ++i) {
        TEST_ASSERT_EQUAL_UINT16(' ', cells[i]);
    }
    TEST_ASSERT_EQUAL_UINT16('x', cells[8]);
}

// Табуляция считается от начала строки, а не от начала окна: иначе при
// прокрутке столбцы съезжали бы вслед за курсором.
static void test_tab_width_is_absolute(void)
{
    load("a\tb");
    seq("F");
    uint32_t col = 0;
    ed_line_col(&s_ed, NULL, &col);
    TEST_ASSERT_EQUAL_UINT32(10, col);  // 1 + 7 клеток табуляции + 1, с единицы
}

static void test_long_line_scrolls_horizontally(void)
{
    load("012345678901234567890123456789012345678901234567890123456789");

    seq("F");
    TEST_ASSERT_EQUAL_UINT32(60, s_ed.cursor);

    int row = 0;
    int col = 0;
    ed_cursor_screen(&s_ed, &row, &col);
    TEST_ASSERT_EQUAL_INT(0, row);
    TEST_ASSERT_EQUAL_INT(EDITOR_COLS - 1, col);
    TEST_ASSERT_EQUAL_UINT32(60 - EDITOR_COLS + 1, s_ed.left);

    // В окне виден хвост строки, а не её начало.
    uint16_t cells[EDITOR_COLS];
    const int n = ed_row(&s_ed, 0, cells, EDITOR_COLS);
    TEST_ASSERT_EQUAL_INT(EDITOR_COLS - 1, n);
    TEST_ASSERT_EQUAL_UINT16('8', cells[0]);
}

// --- вертикальная прокрутка ---

static void test_cursor_below_window_scrolls(void)
{
    load("0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n");

    // Вниз до конца файла.
    for (int i = 0; i < 30; ++i) {
        seq("B");
    }
    uint32_t line = 0;
    ed_line_col(&s_ed, &line, NULL);
    TEST_ASSERT_EQUAL_UINT32(20, line);

    int row = 0;
    int col = 0;
    ed_cursor_screen(&s_ed, &row, &col);
    TEST_ASSERT_EQUAL_INT(EDITOR_ROWS - 1, row);

    // Верхняя строка окна - «4»: 20 строк, а в окне их 16, значит наверху
    // пятая с конца не считая последней. Номер строки проверяется по тексту,
    // а не по смещению: смещение - внутренняя величина, а текст - то, что
    // видит человек.
    uint16_t cells[EDITOR_COLS];
    TEST_ASSERT_TRUE(ed_row(&s_ed, 0, cells, EDITOR_COLS) > 0);
    TEST_ASSERT_EQUAL_UINT16('4', cells[0]);
}

// --- выход и сохранение ---

static void test_quit_clean_file(void)
{
    load("abc");
    TEST_ASSERT_EQUAL_INT(ED_QUIT, ed_input(&s_ed, 0x11));
}

static void test_quit_dirty_needs_confirmation(void)
{
    load("abc");
    type("d");

    TEST_ASSERT_EQUAL_INT(ED_QUIT_DIRTY, ed_input(&s_ed, 0x11));
    TEST_ASSERT_EQUAL_INT(ED_QUIT, ed_input(&s_ed, 0x11));
}

// Подтверждение относится к следующему нажатию: любая другая клавиша его
// снимает, иначе случайный Ctrl-Q через минуту выбрасывал бы из редактора.
static void test_other_key_disarms_quit(void)
{
    load("abc");
    type("d");

    TEST_ASSERT_EQUAL_INT(ED_QUIT_DIRTY, ed_input(&s_ed, 0x11));
    type("e");
    TEST_ASSERT_EQUAL_INT(ED_QUIT_DIRTY, ed_input(&s_ed, 0x11));
}

// Ctrl-C - тот же выход, что Ctrl-Q: на плате кнопка G0 приходит этим байтом.
static void test_ctrl_c_quits(void)
{
    load("abc");
    TEST_ASSERT_EQUAL_INT(ED_QUIT, ed_input(&s_ed, 0x03));
}

static void test_save_event(void)
{
    load("abc");
    TEST_ASSERT_EQUAL_INT(ED_SAVE, ed_input(&s_ed, 0x13));
}

static void test_mark_saved_clears_dirty(void)
{
    load("abc");
    type("d");
    ed_mark_saved(&s_ed);
    TEST_ASSERT_FALSE(ed_dirty(&s_ed));
    TEST_ASSERT_EQUAL_INT(ED_QUIT, ed_input(&s_ed, 0x11));
}

static void test_find_event(void)
{
    load("abc");
    TEST_ASSERT_EQUAL_INT(ED_FIND, ed_input(&s_ed, 0x06));
}

// --- поиск ---

static void test_search_finds_forward(void)
{
    load("one\ntwo\nthree");
    TEST_ASSERT_EQUAL_INT(0, ed_search(&s_ed, "two"));
    TEST_ASSERT_EQUAL_UINT32(4, s_ed.cursor);
}

static void test_search_wraps_to_start(void)
{
    load("one\ntwo\nthree");
    TEST_ASSERT_EQUAL_INT(0, ed_search(&s_ed, "two"));
    TEST_ASSERT_EQUAL_INT(0, ed_search(&s_ed, "one"));
    TEST_ASSERT_EQUAL_UINT32(0, s_ed.cursor);
}

static void test_search_from_cursor_skips_current_match(void)
{
    load("aa");
    TEST_ASSERT_EQUAL_INT(0, ed_search(&s_ed, "a"));
    TEST_ASSERT_EQUAL_UINT32(1, s_ed.cursor);
    TEST_ASSERT_EQUAL_INT(0, ed_search(&s_ed, "a"));
    TEST_ASSERT_EQUAL_UINT32(0, s_ed.cursor);
}

static void test_search_not_found(void)
{
    load("abc");
    TEST_ASSERT_EQUAL_INT(-1, ed_search(&s_ed, "zzz"));
    TEST_ASSERT_EQUAL_INT(-1, ed_search(&s_ed, ""));
}

// --- кириллица и мусор ---

static void test_utf8_insert_and_backspace(void)
{
    load("");
    type("\xD0\x96");  // «Ж»
    TEST_ASSERT_EQUAL_UINT32(2, ed_len(&s_ed));
    TEST_ASSERT_EQUAL_UINT32(2, s_ed.cursor);

    uint32_t line = 0;
    uint32_t col = 0;
    ed_line_col(&s_ed, &line, &col);
    TEST_ASSERT_EQUAL_UINT32(2, col);  // одна клетка, с единицы

    ed_input(&s_ed, 0x7F);
    TEST_ASSERT_EQUAL_UINT32(0, ed_len(&s_ed));
}

static void test_utf8_render(void)
{
    load("\xD0\x9F\xD1\x80\xD0\xB8");  // «При»
    uint16_t cells[EDITOR_COLS];
    const int n = ed_row(&s_ed, 0, cells, EDITOR_COLS);

    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_UINT16(0x041F, cells[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0440, cells[1]);
    TEST_ASSERT_EQUAL_UINT16(0x0438, cells[2]);
}

// Битая последовательность читается как '?' и занимает одну клетку: иначе
// остальные клетки съехали бы, а курсор встал бы внутрь буквы.
static void test_invalid_utf8_is_one_cell(void)
{
    load("a\xFFz");
    uint16_t cells[EDITOR_COLS];
    const int n = ed_row(&s_ed, 0, cells, EDITOR_COLS);

    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_UINT16('a', cells[0]);
    TEST_ASSERT_EQUAL_UINT16('?', cells[1]);
    TEST_ASSERT_EQUAL_UINT16('z', cells[2]);
}

static void test_control_bytes_render_as_question(void)
{
    load("a\x01z");
    uint16_t cells[EDITOR_COLS];
    const int n = ed_row(&s_ed, 0, cells, EDITOR_COLS);

    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_UINT16('?', cells[1]);
}

// --- ёмкость и окно ---

// Буфер полон: вставка не проходит, но и половина символа не записывается.
static void test_full_buffer_refuses_insert(void)
{
    static char small[4];
    ed_init(&s_ed, small, sizeof(small));
    ed_load(&s_ed, 0);

    type("abcd");
    TEST_ASSERT_EQUAL_UINT32(4, ed_len(&s_ed));

    TEST_ASSERT_EQUAL_INT(ED_BELL, ed_input(&s_ed, 'e'));
    text_is("abcd");
}

// Строки за концом файла - пустые, а не «продолжение последней».
static void test_rows_after_last_line_are_empty(void)
{
    load("a\nb");
    uint16_t cells[EDITOR_COLS];

    TEST_ASSERT_EQUAL_INT(1, ed_row(&s_ed, 0, cells, EDITOR_COLS));
    TEST_ASSERT_EQUAL_INT(1, ed_row(&s_ed, 1, cells, EDITOR_COLS));
    TEST_ASSERT_EQUAL_INT(0, ed_row(&s_ed, 2, cells, EDITOR_COLS));
}

// Пустой файл - это одна пустая строка, а не «экрана нет».
static void test_empty_file_is_one_empty_row(void)
{
    load("");
    uint16_t cells[EDITOR_COLS];
    TEST_ASSERT_EQUAL_INT(0, ed_row(&s_ed, 0, cells, EDITOR_COLS));
    TEST_ASSERT_EQUAL_INT(0, ed_row(&s_ed, 1, cells, EDITOR_COLS));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_load_empty_file);
    RUN_TEST(test_load_crlf_becomes_lf);
    RUN_TEST(test_lone_cr_is_kept);
    RUN_TEST(test_load_strips_bom);
    RUN_TEST(test_load_clears_dirty);

    RUN_TEST(test_type_ascii);
    RUN_TEST(test_backspace_removes_previous);
    RUN_TEST(test_backspace_at_start_is_bell);
    RUN_TEST(test_enter_splits_line);
    RUN_TEST(test_backspace_joins_lines);
    RUN_TEST(test_delete_joins_lines);
    RUN_TEST(test_delete_at_end_is_bell);

    RUN_TEST(test_home_and_end);
    RUN_TEST(test_left_at_line_start_goes_to_previous_end);
    RUN_TEST(test_up_down_keep_goal_column);
    RUN_TEST(test_up_at_first_line_does_not_move);
    RUN_TEST(test_right_at_end_does_not_move);

    RUN_TEST(test_tab_expands_to_cells);
    RUN_TEST(test_tab_width_is_absolute);
    RUN_TEST(test_long_line_scrolls_horizontally);
    RUN_TEST(test_cursor_below_window_scrolls);

    RUN_TEST(test_quit_clean_file);
    RUN_TEST(test_quit_dirty_needs_confirmation);
    RUN_TEST(test_other_key_disarms_quit);
    RUN_TEST(test_ctrl_c_quits);
    RUN_TEST(test_save_event);
    RUN_TEST(test_mark_saved_clears_dirty);
    RUN_TEST(test_find_event);

    RUN_TEST(test_search_finds_forward);
    RUN_TEST(test_search_wraps_to_start);
    RUN_TEST(test_search_from_cursor_skips_current_match);
    RUN_TEST(test_search_not_found);

    RUN_TEST(test_utf8_insert_and_backspace);
    RUN_TEST(test_utf8_render);
    RUN_TEST(test_invalid_utf8_is_one_cell);
    RUN_TEST(test_control_bytes_render_as_question);

    RUN_TEST(test_full_buffer_refuses_insert);
    RUN_TEST(test_rows_after_last_line_are_empty);
    RUN_TEST(test_empty_file_is_one_empty_row);

    return UNITY_END();
}
