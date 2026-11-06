// Тесты редактора строки на хосте.
//
// Почему здесь, а не на плате: редактирование - это десяток сочетаний
// (курсор в середине кириллицы, Ctrl-W после пробела, ↑ с непустым черновиком),
// и проверять их руками на экране 320x240 по минуте на каждое бессмысленно.
// Тест подаёт code point и смотрит событие - ровно то, что увидит приложение.

#include <unity.h>

#include <stdint.h>
#include <string.h>

#include "history.h"
#include "lineedit.h"

void setUp(void)
{
    le_init();
}

void tearDown(void)
{
}

// Скормить ASCII-строку посимвольно.
static void type(const char* s)
{
    while (*s != '\0') {
        le_input((uint8_t)*s++);
    }
}

// Скормить escape-последовательность целиком.
static le_event escape(const char* seq)
{
    le_event ev = LE_NONE;
    while (*seq != '\0') {
        ev = le_input((uint8_t)*seq++);
    }
    return ev;
}

#define UP    escape("\x1b[A")
#define DOWN  escape("\x1b[B")
#define RIGHT escape("\x1b[C")
#define LEFT  escape("\x1b[D")
#define HOME_S escape("\x1b[H")
#define END_S escape("\x1b[F")
#define DELKEY escape("\x1b[3~")

// --- набор текста ---

void test_empty_after_init(void)
{
    TEST_ASSERT_EQUAL_STRING("", le_line());
    TEST_ASSERT_EQUAL_size_t(0, le_len());
    TEST_ASSERT_EQUAL_size_t(0, le_cursor());
    TEST_ASSERT_EQUAL_size_t(0, le_cols());
}

void test_type_ascii(void)
{
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input('l'));
    type("s");

    TEST_ASSERT_EQUAL_STRING("ls", le_line());
    TEST_ASSERT_EQUAL_size_t(2, le_len());
    TEST_ASSERT_EQUAL_size_t(2, le_cursor());
    TEST_ASSERT_EQUAL_size_t(2, le_cols());
    TEST_ASSERT_EQUAL_size_t(2, le_cursor_col());
}

void test_type_cyrillic_counts_bytes_and_cells(void)
{
    // «Ж» - U+0416: два байта в буфере, одна клетка на экране.
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x416));

    TEST_ASSERT_EQUAL_size_t(2, le_len());
    TEST_ASSERT_EQUAL_size_t(1, le_cols());
    TEST_ASSERT_EQUAL_size_t(1, le_cursor_col());
}

void test_mixed_width(void)
{
    le_input('a');
    le_input(0x416);
    le_input('b');

    TEST_ASSERT_EQUAL_size_t(4, le_len());
    TEST_ASSERT_EQUAL_size_t(3, le_cols());
}

// --- клетки строки ---

void test_cell_cp_addresses_whole_chars(void)
{
    le_input('a');
    le_input(0x416);
    le_input('b');

    // Клетка, а не байт: у «Ж» один адрес, хотя байта два.
    TEST_ASSERT_EQUAL_UINT32('a', le_cell_cp(0));
    TEST_ASSERT_EQUAL_UINT32(0x416, le_cell_cp(1));
    TEST_ASSERT_EQUAL_UINT32('b', le_cell_cp(2));
    TEST_ASSERT_EQUAL_UINT32(0, le_cell_cp(3));
}

void test_cell_cp_three_byte_char(void)
{
    le_input(0x20AC);  // €
    TEST_ASSERT_EQUAL_size_t(1, le_cols());
    TEST_ASSERT_EQUAL_UINT32(0x20AC, le_cell_cp(0));
    TEST_ASSERT_EQUAL_UINT32(0, le_cell_cp(1));
}

void test_cell_cp_of_empty_line(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, le_cell_cp(0));
}

void test_overlong_control_ignored(void)
{
    TEST_ASSERT_EQUAL_INT(LE_NONE, le_input('\t'));
    TEST_ASSERT_EQUAL_INT(LE_NONE, le_input(0x1A));  // Ctrl-Z
    TEST_ASSERT_EQUAL_STRING("", le_line());
}

void test_overflow(void)
{
    for (size_t i = 0; i < LINEEDIT_CAP - 1; ++i) {
        TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input('a'));
    }
    TEST_ASSERT_EQUAL_size_t(LINEEDIT_CAP - 1, le_len());

    TEST_ASSERT_EQUAL_INT(LE_BELL, le_input('a'));
    TEST_ASSERT_EQUAL_size_t(LINEEDIT_CAP - 1, le_len());
}

// --- удаление ---

void test_backspace(void)
{
    type("abc");
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x7F));

    TEST_ASSERT_EQUAL_STRING("ab", le_line());
    TEST_ASSERT_EQUAL_size_t(2, le_cursor());
}

void test_backspace_cyrillic_whole_char(void)
{
    le_input(0x416);
    le_input(0x416);

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x7F));
    TEST_ASSERT_EQUAL_size_t(2, le_len());
    TEST_ASSERT_EQUAL_size_t(1, le_cols());
}

void test_backspace_at_start_bells(void)
{
    TEST_ASSERT_EQUAL_INT(LE_BELL, le_input(0x7F));
    TEST_ASSERT_EQUAL_INT(LE_BELL, le_input(0x08));
}

void test_delete_forward(void)
{
    type("abc");
    LEFT;
    LEFT;

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, DELKEY);
    TEST_ASSERT_EQUAL_STRING("ac", le_line());
    TEST_ASSERT_EQUAL_size_t(1, le_cursor());
}

void test_delete_at_end_bells(void)
{
    type("ab");
    TEST_ASSERT_EQUAL_INT(LE_BELL, DELKEY);
}

void test_ctrl_d_deletes_under_cursor(void)
{
    type("abc");
    LEFT;
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x04));
    TEST_ASSERT_EQUAL_STRING("ab", le_line());
}

// --- движение курсора ---

void test_arrows(void)
{
    type("abc");

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, LEFT);
    TEST_ASSERT_EQUAL_size_t(2, le_cursor());
    TEST_ASSERT_EQUAL_size_t(2, le_cursor_col());

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, RIGHT);
    TEST_ASSERT_EQUAL_size_t(3, le_cursor());

    TEST_ASSERT_EQUAL_INT(LE_BELL, RIGHT);
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, HOME_S);
    TEST_ASSERT_EQUAL_size_t(0, le_cursor());
    TEST_ASSERT_EQUAL_INT(LE_BELL, LEFT);
}

void test_arrow_skips_cyrillic_bytes(void)
{
    le_input(0x416);
    le_input(0x416);

    LEFT;
    TEST_ASSERT_EQUAL_size_t(2, le_cursor());
    TEST_ASSERT_EQUAL_size_t(1, le_cursor_col());

    LEFT;
    TEST_ASSERT_EQUAL_size_t(0, le_cursor());
}

void test_home_end_numeric_forms(void)
{
    type("abc");

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, escape("\x1b[1~"));
    TEST_ASSERT_EQUAL_size_t(0, le_cursor());

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, escape("\x1b[4~"));
    TEST_ASSERT_EQUAL_size_t(3, le_cursor());
}

void test_home_end_at_edges_are_quiet(void)
{
    // Ничего не изменилось - событие NONE, а не CHANGED: перерисовка зря.
    TEST_ASSERT_EQUAL_INT(LE_NONE, HOME_S);
    type("ab");
    TEST_ASSERT_EQUAL_INT(LE_NONE, END_S);
}

void test_ctrl_a_and_e(void)
{
    type("abc");

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x01));
    TEST_ASSERT_EQUAL_size_t(0, le_cursor());

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x05));
    TEST_ASSERT_EQUAL_size_t(3, le_cursor());
}

// --- вырезание ---

void test_ctrl_k_kills_to_end(void)
{
    type("abcdef");
    HOME_S;
    RIGHT;
    RIGHT;

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x0B));
    TEST_ASSERT_EQUAL_STRING("ab", le_line());
    TEST_ASSERT_EQUAL_size_t(2, le_cursor());
}

void test_ctrl_k_at_end_bells(void)
{
    type("ab");
    TEST_ASSERT_EQUAL_INT(LE_BELL, le_input(0x0B));
}

void test_ctrl_u_kills_to_start(void)
{
    type("abcdef");
    HOME_S;
    RIGHT;
    RIGHT;

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x15));
    TEST_ASSERT_EQUAL_STRING("cdef", le_line());
    TEST_ASSERT_EQUAL_size_t(0, le_cursor());
}

void test_ctrl_u_at_start_bells(void)
{
    type("ab");
    HOME_S;
    TEST_ASSERT_EQUAL_INT(LE_BELL, le_input(0x15));
}

void test_ctrl_w_kills_word(void)
{
    type("echo privet mir");
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x17));
    TEST_ASSERT_EQUAL_STRING("echo privet ", le_line());

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x17));
    TEST_ASSERT_EQUAL_STRING("echo ", le_line());
}

void test_ctrl_w_eats_trailing_spaces(void)
{
    type("echo   ");
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x17));
    TEST_ASSERT_EQUAL_STRING("", le_line());
}

void test_ctrl_w_at_start_bells(void)
{
    TEST_ASSERT_EQUAL_INT(LE_BELL, le_input(0x17));
}

void test_ctrl_l_asks_redraw(void)
{
    type("ab");
    TEST_ASSERT_EQUAL_INT(LE_REDRAW, le_input(0x0C));
    // Строка при этом не меняется: перерисовка - не правка.
    TEST_ASSERT_EQUAL_STRING("ab", le_line());
    TEST_ASSERT_EQUAL_size_t(2, le_cursor());
}

// --- история ---

void test_history_up_down_with_draft(void)
{
    TEST_ASSERT_TRUE(history_push("alpha"));
    TEST_ASSERT_TRUE(history_push("beta"));

    type("dr");
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, UP);
    TEST_ASSERT_EQUAL_STRING("beta", le_line());
    TEST_ASSERT_EQUAL_size_t(4, le_cursor());

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, UP);
    TEST_ASSERT_EQUAL_STRING("alpha", le_line());

    TEST_ASSERT_EQUAL_INT(LE_BELL, UP);

    TEST_ASSERT_EQUAL_INT(LE_CHANGED, DOWN);
    TEST_ASSERT_EQUAL_STRING("beta", le_line());

    // За пределами списка возвращается черновик, который был до листания.
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, DOWN);
    TEST_ASSERT_EQUAL_STRING("dr", le_line());

    TEST_ASSERT_EQUAL_INT(LE_BELL, DOWN);
}

void test_history_empty_bells(void)
{
    TEST_ASSERT_EQUAL_INT(LE_BELL, UP);
    TEST_ASSERT_EQUAL_INT(LE_BELL, DOWN);
}

void test_edit_after_recall_detaches_from_history(void)
{
    TEST_ASSERT_TRUE(history_push("one"));
    TEST_ASSERT_TRUE(history_push("two"));

    UP;
    TEST_ASSERT_EQUAL_STRING("two", le_line());

    type("!");
    TEST_ASSERT_EQUAL_STRING("two!", le_line());

    // Строка больше не «та запись»: вниз листать некуда.
    TEST_ASSERT_EQUAL_INT(LE_BELL, DOWN);
}

// --- события строки ---

void test_enter_submits_and_stores(void)
{
    type("ls");
    TEST_ASSERT_EQUAL_INT(LE_SUBMIT, le_input('\r'));

    TEST_ASSERT_EQUAL_INT(1, history_count());
    TEST_ASSERT_EQUAL_STRING("ls", history_get(0));
}

void test_enter_on_empty_not_stored(void)
{
    TEST_ASSERT_EQUAL_INT(LE_SUBMIT, le_input('\n'));
    TEST_ASSERT_EQUAL_INT(0, history_count());
}

void test_ctrl_c_interrupts(void)
{
    type("rm -rf");
    TEST_ASSERT_EQUAL_INT(LE_INTR, le_input(0x03));
}

void test_ctrl_d_eof_only_when_empty(void)
{
    // На пустой строке - конец ввода.
    TEST_ASSERT_EQUAL_INT(LE_EOF, le_input(0x04));

    // На непустой - удаление под курсором, как Delete. В конце строки под
    // курсором ничего нет, поэтому там Ctrl-D только звенит.
    type("ab");
    TEST_ASSERT_EQUAL_INT(LE_BELL, le_input(0x04));

    HOME_S;
    TEST_ASSERT_EQUAL_INT(LE_CHANGED, le_input(0x04));
    TEST_ASSERT_EQUAL_STRING("b", le_line());
}

void test_reset_clears_buffer(void)
{
    type("abc");
    le_reset();

    TEST_ASSERT_EQUAL_STRING("", le_line());
    TEST_ASSERT_EQUAL_size_t(0, le_len());
    TEST_ASSERT_EQUAL_size_t(0, le_cursor());
}

void test_set_line(void)
{
    le_set_line("help");
    TEST_ASSERT_EQUAL_STRING("help", le_line());
    TEST_ASSERT_EQUAL_size_t(4, le_cursor());
    TEST_ASSERT_EQUAL_size_t(4, le_cursor_col());

    le_set_line(NULL);
    TEST_ASSERT_EQUAL_STRING("", le_line());
}

void test_set_line_truncates_to_cap(void)
{
    char big[LINEEDIT_CAP + 32];
    memset(big, 'q', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    le_set_line(big);
    TEST_ASSERT_EQUAL_size_t(LINEEDIT_CAP - 1, le_len());
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_empty_after_init);
    RUN_TEST(test_type_ascii);
    RUN_TEST(test_type_cyrillic_counts_bytes_and_cells);
    RUN_TEST(test_mixed_width);
    RUN_TEST(test_cell_cp_addresses_whole_chars);
    RUN_TEST(test_cell_cp_three_byte_char);
    RUN_TEST(test_cell_cp_of_empty_line);
    RUN_TEST(test_overlong_control_ignored);
    RUN_TEST(test_overflow);

    RUN_TEST(test_backspace);
    RUN_TEST(test_backspace_cyrillic_whole_char);
    RUN_TEST(test_backspace_at_start_bells);
    RUN_TEST(test_delete_forward);
    RUN_TEST(test_delete_at_end_bells);
    RUN_TEST(test_ctrl_d_deletes_under_cursor);

    RUN_TEST(test_arrows);
    RUN_TEST(test_arrow_skips_cyrillic_bytes);
    RUN_TEST(test_home_end_numeric_forms);
    RUN_TEST(test_home_end_at_edges_are_quiet);
    RUN_TEST(test_ctrl_a_and_e);

    RUN_TEST(test_ctrl_k_kills_to_end);
    RUN_TEST(test_ctrl_k_at_end_bells);
    RUN_TEST(test_ctrl_u_kills_to_start);
    RUN_TEST(test_ctrl_u_at_start_bells);
    RUN_TEST(test_ctrl_w_kills_word);
    RUN_TEST(test_ctrl_w_eats_trailing_spaces);
    RUN_TEST(test_ctrl_w_at_start_bells);
    RUN_TEST(test_ctrl_l_asks_redraw);

    RUN_TEST(test_history_up_down_with_draft);
    RUN_TEST(test_history_empty_bells);
    RUN_TEST(test_edit_after_recall_detaches_from_history);

    RUN_TEST(test_enter_submits_and_stores);
    RUN_TEST(test_enter_on_empty_not_stored);
    RUN_TEST(test_ctrl_c_interrupts);
    RUN_TEST(test_ctrl_d_eof_only_when_empty);
    RUN_TEST(test_reset_clears_buffer);
    RUN_TEST(test_set_line);
    RUN_TEST(test_set_line_truncates_to_cap);

    return UNITY_END();
}
