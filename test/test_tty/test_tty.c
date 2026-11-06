// Тесты line discipline на хосте.
//
// Почему это стоит проверять на маке, а не на плате: набор текста - это
// последовательность байтов, и ошибка в ней видна только при определённом
// сочетании (Backspace после кириллицы, Ctrl-D на пустой строке,
// переполнение буфера). На плате такие сочетания набирать руками по минуте,
// а здесь они стоят одну строку.
//
// Тест кормит tty байты и смотрит события - ровно то, что увидит приложение.

#include <unity.h>

#include <stdint.h>
#include <string.h>

#include "tty.h"

void setUp(void)
{
    tty_init();
}

void tearDown(void)
{
}

// Скормить строку байтов, вернуть событие от последнего байта.
static tty_result feed(const char* s)
{
    tty_result r;
    r.event = TTY_EV_NONE;
    r.cp = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p != 0; ++p) {
        r = tty_input(*p);
    }
    return r;
}

// Сколько раз встретилось событие за время подачи строки.
static int count_events(const char* s, tty_event want)
{
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p != 0; ++p) {
        if (tty_input(*p).event == want) {
            ++n;
        }
    }
    return n;
}

// --- канонический режим ---

// До Enter ни одного LINE: строка копится, а не отдаётся по символу. Это
// главное отличие канона от raw, и на нём держится построчный ввод.
static void test_canon_holds_until_enter(void)
{
    TEST_ASSERT_EQUAL_INT(0, count_events("abc", TTY_EV_LINE));

    const tty_result r = tty_input('\r');
    TEST_ASSERT_EQUAL_INT(TTY_EV_LINE, r.event);
    TEST_ASSERT_EQUAL_STRING("abc", tty_line());
    TEST_ASSERT_EQUAL_INT(3, (int)tty_line_len());
}

// \n тоже завершает строку: терминал мака присылает \r, но данные из скрипта
// или из другой программы придут с \n, и принимать только один нельзя.
static void test_canon_lf_also_finishes_line(void)
{
    feed("echo");
    TEST_ASSERT_EQUAL_INT(TTY_EV_LINE, tty_input('\n').event);
    TEST_ASSERT_EQUAL_STRING("echo", tty_line());
}

// Эхо: каждый печатный байт даёт CHAR с этим же кодом - приложение по нему
// рисует символ.
static void test_canon_echo_per_char(void)
{
    const tty_result r = tty_input('A');
    TEST_ASSERT_EQUAL_INT(TTY_EV_CHAR, r.event);
    TEST_ASSERT_EQUAL_HEX32('A', r.cp);
}

// Backspace приходит как 0x7F (так шлёт терминал мака) и как 0x08 (старый
// стандарт). Принимать надо оба, иначе клавиша работает «через раз».
static void test_backspace_both_codes(void)
{
    feed("ab");
    TEST_ASSERT_EQUAL_INT(TTY_EV_ERASE, tty_input(0x7F).event);
    TEST_ASSERT_EQUAL_STRING("a", tty_line());

    TEST_ASSERT_EQUAL_INT(TTY_EV_ERASE, tty_input(0x08).event);
    TEST_ASSERT_EQUAL_STRING("", tty_line());
}

// Backspace в начале строки: стирать нечего, событие BELL. Строка при этом
// не портится.
static void test_backspace_at_start_is_bell(void)
{
    TEST_ASSERT_EQUAL_INT(TTY_EV_BELL, tty_input(0x7F).event);
    TEST_ASSERT_EQUAL_INT(0, (int)tty_line_len());
}

// Backspace после кириллицы обязан стереть всю букву. Если откатывать по
// байту, в строке останется половина последовательности, и следующая буква
// склеится с ней - на экране это выглядит как чужая буква посреди слова.
static void test_backspace_removes_whole_cyrillic(void)
{
    feed("\xD0\x96");  // «Ж» - два байта, одна клетка
    TEST_ASSERT_EQUAL_INT(2, (int)tty_line_len());

    TEST_ASSERT_EQUAL_INT(TTY_EV_ERASE, tty_input(0x7F).event);
    TEST_ASSERT_EQUAL_INT(0, (int)tty_line_len());
    TEST_ASSERT_EQUAL_STRING("", tty_line());
}

// Смешанный случай: ASCII + кириллица, стираем по одному. Ловит ошибку
// «откатить на длину последней последовательности», которая на ASCII верна.
static void test_backspace_mixed_ascii_and_cyrillic(void)
{
    feed("a\xD0\x96\xD0\xB1");  // "aЖб"
    TEST_ASSERT_EQUAL_INT(5, (int)tty_line_len());

    tty_input(0x7F);
    TEST_ASSERT_EQUAL_INT(3, (int)tty_line_len());

    tty_input(0x7F);
    TEST_ASSERT_EQUAL_INT(1, (int)tty_line_len());
    TEST_ASSERT_EQUAL_STRING("a", tty_line());

    tty_input(0x7F);
    TEST_ASSERT_EQUAL_INT(0, (int)tty_line_len());
}

// Ctrl-C выбрасывает набранное и сообщает INTR: прерывать надо до того, как
// программа получит строку, а не после.
static void test_ctrl_c_discards_line(void)
{
    feed("rm -rf /");
    TEST_ASSERT_EQUAL_INT(TTY_EV_INTR, tty_input(0x03).event);
    TEST_ASSERT_EQUAL_INT(0, (int)tty_pending_len());
}

// Ctrl-D на пустой строке - конец ввода.
static void test_ctrl_d_on_empty_is_eof(void)
{
    TEST_ASSERT_EQUAL_INT(TTY_EV_EOF, tty_input(0x04).event);
}

// Ctrl-D на непустой - отдать накопленное, как в настоящем терминале.
static void test_ctrl_d_with_text_flushes_line(void)
{
    feed("exit");
    TEST_ASSERT_EQUAL_INT(TTY_EV_LINE, tty_input(0x04).event);
    TEST_ASSERT_EQUAL_STRING("exit", tty_line());
}

// После отданной строки начинается новая, а не продолжение старой.
static void test_next_line_starts_clean(void)
{
    feed("first");
    tty_input('\r');

    // Предыдущая строка ещё читается - до следующего байта.
    TEST_ASSERT_EQUAL_STRING("first", tty_line());

    // Первый байт новой строки начинает её с нуля, а не дописывает старую.
    tty_input('s');
    TEST_ASSERT_EQUAL_INT(1, (int)tty_pending_len());
    TEST_ASSERT_EQUAL_STRING("s", tty_pending());
}

// --- raw ---

// Главное отличие: в raw нет строк. Каждый байт - отдельное событие CHAR.
static void test_raw_emits_per_byte(void)
{
    tty_set_mode(TTY_MODE_RAW);
    TEST_ASSERT_EQUAL_INT(0, count_events("abc\r", TTY_EV_LINE));
    TEST_ASSERT_EQUAL_INT(3, count_events("abc", TTY_EV_CHAR));
}

// В raw \r приходит как обычный байт: терминал не превращает его в конец
// строки, это дело программы.
static void test_raw_cr_is_plain_byte(void)
{
    tty_set_mode(TTY_MODE_RAW);
    const tty_result r = tty_input('\r');
    TEST_ASSERT_EQUAL_INT(TTY_EV_CHAR, r.event);
    TEST_ASSERT_EQUAL_HEX32('\r', r.cp);
}

// Кириллица в raw тоже один символ, а не два байта: программа не должна
// собирать UTF-8 сама.
static void test_raw_cyrillic_one_event(void)
{
    tty_set_mode(TTY_MODE_RAW);
    TEST_ASSERT_EQUAL_INT(TTY_EV_NONE, tty_input(0xD0).event);
    const tty_result r = tty_input(0x96);
    TEST_ASSERT_EQUAL_INT(TTY_EV_CHAR, r.event);
    TEST_ASSERT_EQUAL_HEX32(0x0416, r.cp);
}

// Из raw есть выход: Ctrl-C работает в обоих режимах, иначе зависшую
// программу было бы не остановить.
static void test_raw_ctrl_c_still_works(void)
{
    tty_set_mode(TTY_MODE_RAW);
    TEST_ASSERT_EQUAL_INT(TTY_EV_INTR, tty_input(0x03).event);
}

// В raw буфер строки не ведётся: отдавать программе нечего, а держать в
// памяти набранное «на всякий случай» - значит однажды показать это в каноне.
static void test_raw_keeps_no_buffer(void)
{
    tty_set_mode(TTY_MODE_RAW);
    feed("hello");
    TEST_ASSERT_EQUAL_INT(0, (int)tty_pending_len());
}

// --- переключение режима ---

// Смена режима выбрасывает буфер: половина команды, набранная в каноне, не
// должна достаться программе в raw.
static void test_mode_switch_clears_buffer(void)
{
    feed("half");
    tty_set_mode(TTY_MODE_RAW);
    TEST_ASSERT_EQUAL_INT(0, (int)tty_pending_len());
}

// --- переполнение ---

// Длинная строка принимается до предела, а следующая нажатие даёт BELL, а не
// молчаливую потерю: иначе команда обрезалась бы без единого признака.
static void test_overflow_is_bell(void)
{
    for (int i = 0; i < TTY_LINE_CAP - 1; ++i) {
        TEST_ASSERT_EQUAL_INT(TTY_EV_CHAR, tty_input('x').event);
    }
    TEST_ASSERT_EQUAL_INT(TTY_LINE_CAP - 1, (int)tty_line_len());

    TEST_ASSERT_EQUAL_INT(TTY_EV_BELL, tty_input('y').event);
    TEST_ASSERT_EQUAL_INT(TTY_LINE_CAP - 1, (int)tty_line_len());
}

// Битая последовательность не оставляет в буфере огрызок: иначе строка
// оказалась бы короче набранного и с мусором внутри.
static void test_invalid_utf8_leaves_no_residue(void)
{
    feed("ab");
    // 0xD0 - головка двухбайтовой буквы, 0x41 - не продолжение.
    tty_input(0xD0);
    TEST_ASSERT_EQUAL_INT(3, (int)tty_line_len());

    tty_input(0x41);
    TEST_ASSERT_EQUAL_INT(3, (int)tty_line_len());
    TEST_ASSERT_EQUAL_STRING("abA", tty_line());
}

// --- прочее ---

// Управляющие байты, которых мы пока не поддерживаем (стрелки приходят как
// ESC-последовательности), не попадают в строку: иначе команда ломалась бы
// при попытке нажать вверх.
static void test_unsupported_control_ignored(void)
{
    feed("ab");
    TEST_ASSERT_EQUAL_INT(TTY_EV_NONE, tty_input(0x1B).event);
    TEST_ASSERT_EQUAL_STRING("ab", tty_line());
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_canon_holds_until_enter);
    RUN_TEST(test_canon_lf_also_finishes_line);
    RUN_TEST(test_canon_echo_per_char);
    RUN_TEST(test_backspace_both_codes);
    RUN_TEST(test_backspace_at_start_is_bell);
    RUN_TEST(test_backspace_removes_whole_cyrillic);
    RUN_TEST(test_backspace_mixed_ascii_and_cyrillic);
    RUN_TEST(test_ctrl_c_discards_line);
    RUN_TEST(test_ctrl_d_on_empty_is_eof);
    RUN_TEST(test_ctrl_d_with_text_flushes_line);
    RUN_TEST(test_next_line_starts_clean);

    RUN_TEST(test_raw_emits_per_byte);
    RUN_TEST(test_raw_cr_is_plain_byte);
    RUN_TEST(test_raw_cyrillic_one_event);
    RUN_TEST(test_raw_ctrl_c_still_works);
    RUN_TEST(test_raw_keeps_no_buffer);

    RUN_TEST(test_mode_switch_clears_buffer);

    RUN_TEST(test_overflow_is_bell);
    RUN_TEST(test_invalid_utf8_leaves_no_residue);
    RUN_TEST(test_unsupported_control_ignored);

    return UNITY_END();
}
