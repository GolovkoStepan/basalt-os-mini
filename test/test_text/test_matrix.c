// Тесты текстовой матрицы на хосте.
//
// Почему без заглушки драйвера: матрица не зовёт lcd_flush, она рисует полосу
// в буфер, который ей дали. Значит проверять можно пиксели, а не «драйвер
// вызвали столько раз» - и это сильнее: видно, что буква попала в нужное
// место и не перевёрнута.
//
// Что здесь ловится, а на плате стоило бы вечера:
//   - перенос на 53-м символе и прокрутка на 17-й строке;
//   - порядок бит в глифе (при чтении наоборот буквы зеркальны);
//   - dirty-строки: печать одного символа не должна помечать все 17;
//   - положение курсора: подчёркивание под клеткой, а не поверх буквы.

#include <unity.h>

#include <stdint.h>
#include <string.h>

#include "matrix.h"

#define FG 0xFFFFu
#define BG 0x0000u

// Полоса одной строки - столько же, сколько просит драйвер.
static uint16_t s_strip[TEXT_STRIP_PIXELS];

// Пиксель из нарисованной полосы.
static uint16_t pixel(int x, int y)
{
    return s_strip[(size_t)y * TEXT_STRIP_W + (size_t)x];
}

// Левый верхний угол клетки в пикселях полосы.
static int cell_x(int col)
{
    return TEXT_MARGIN_X + col * TEXT_CELL_W;
}

static void render(int row)
{
    memset(s_strip, 0xAA, sizeof(s_strip));
    text_render_row(row, s_strip);
}

static int count_dirty(void)
{
    int n = 0;
    for (int row = 0; row < TEXT_ROWS; ++row) {
        if (text_row_dirty(row)) {
            ++n;
        }
    }
    return n;
}

static void clean_all(void)
{
    for (int row = 0; row < TEXT_ROWS; ++row) {
        text_row_clean(row);
    }
}

void setUp(void)
{
    text_init();
    text_set_colors(FG, BG);
    clean_all();
}

void tearDown(void)
{
}

// --- геометрия ---

// Числа в matrix.h должны сходиться с панелью без остатка: 53 клетки по 6 и
// поле по пикселю - ровно 320, 17 строк по 10 - ровно 170. Если это перестанет
// быть так, текст поедет относительно края, и виноват будет не драйвер.
static void test_geometry_fits_panel(void)
{
    TEST_ASSERT_EQUAL_INT(53, TEXT_COLS);
    TEST_ASSERT_EQUAL_INT(17, TEXT_ROWS);
    TEST_ASSERT_EQUAL_INT(320, TEXT_STRIP_W);
    TEST_ASSERT_EQUAL_INT(TEXT_CELL_H * TEXT_ROWS, 170);
    TEST_ASSERT_TRUE(TEXT_GLYPH_W < TEXT_CELL_W);
    TEST_ASSERT_TRUE(TEXT_GLYPH_H + TEXT_GLYPH_DY < TEXT_CELL_H);
}

// --- вывод символов ---

static void test_putc_advances_cursor(void)
{
    TEST_ASSERT_EQUAL_INT(0, text_cursor_col());

    text_putc('A');
    TEST_ASSERT_EQUAL_INT(1, text_cursor_col());
    TEST_ASSERT_EQUAL_INT(0, text_cursor_row());

    text_puts("BC");
    TEST_ASSERT_EQUAL_INT(3, text_cursor_col());
}

static void test_newline_resets_column(void)
{
    text_puts("abc\n");
    TEST_ASSERT_EQUAL_INT(0, text_cursor_col());
    TEST_ASSERT_EQUAL_INT(1, text_cursor_row());
}

static void test_carriage_return_only_moves_column(void)
{
    text_puts("abc\r");
    TEST_ASSERT_EQUAL_INT(0, text_cursor_col());
    TEST_ASSERT_EQUAL_INT(0, text_cursor_row());
}

// 53-й символ должен уйти на следующую строку, а не встать в 54-ю клетку.
static void test_wrap_after_last_column(void)
{
    for (int i = 0; i < TEXT_COLS; ++i) {
        text_putc('x');
    }
    TEST_ASSERT_EQUAL_INT(0, text_cursor_col());
    TEST_ASSERT_EQUAL_INT(1, text_cursor_row());
}

// Прокрутка: печать в последней строке сдвигает всё на строку вверх.
static void test_scroll_on_last_row(void)
{
    text_cursor_at(0, 0);
    text_puts("first");

    text_cursor_at(0, TEXT_ROWS - 1);
    text_puts("last\n");

    TEST_ASSERT_EQUAL_INT(TEXT_ROWS - 1, text_cursor_row());
    TEST_ASSERT_EQUAL_INT(0, text_cursor_col());

    // Верхняя строка была очищена при прокрутке, а не осталась с "first":
    // рисуем её и убеждаемся, что светлых пикселей нет.
    render(0);
    for (int i = 0; i < TEXT_STRIP_PIXELS; ++i) {
        TEST_ASSERT_EQUAL_HEX16(BG, s_strip[i]);
    }
}

// Прокрутка не должна терять строку, которая была под первой: она
// поднимается наверх.
static void test_scroll_moves_content_up(void)
{
    text_cursor_at(0, 1);
    text_puts("shift");
    text_cursor_at(0, TEXT_ROWS - 1);
    text_putc('\n');

    // После прокрутки "shift" должна оказаться в первой строке.
    render(0);
    int lit = 0;
    for (int i = 0; i < TEXT_STRIP_PIXELS; ++i) {
        if (s_strip[i] == FG) {
            ++lit;
        }
    }
    TEST_ASSERT_TRUE(lit > 0);
}

// Управляющие байты и всё вне 0x20..0x7E игнорируются: шрифта для них нет,
// но и курсор двигать на них нельзя - иначе UTF-8 сдвинул бы текст.
static void test_unprintable_ignored(void)
{
    text_putc((char)0x01);
    text_putc((char)0x7F);
    text_putc((char)0xC0);
    TEST_ASSERT_EQUAL_INT(0, text_cursor_col());
}

static void test_tab_to_next_stop(void)
{
    text_putc('\t');
    TEST_ASSERT_EQUAL_INT(8, text_cursor_col());

    text_putc('\t');
    TEST_ASSERT_EQUAL_INT(16, text_cursor_col());
}

static void test_backspace_stops_at_line_start(void)
{
    text_putc('\b');
    TEST_ASSERT_EQUAL_INT(0, text_cursor_col());

    text_puts("ab\b");
    TEST_ASSERT_EQUAL_INT(1, text_cursor_col());
}

// --- отрисовка ---

// Пробел не рисует ничего: полоса остаётся фоном.
static void test_space_draws_nothing(void)
{
    text_putc(' ');
    render(0);
    for (int i = 0; i < TEXT_STRIP_PIXELS; ++i) {
        TEST_ASSERT_EQUAL_HEX16(BG, s_strip[i]);
    }
}

// Порядок бит. У 'L' левый столбец закрашен сверху донизу, а правая часть
// пуста до нижней строки. Если биты читать наоборот, закрашенным окажется
// правый столбец - то есть буква выйдет зеркальной.
static void test_glyph_bit_order(void)
{
    text_putc('L');
    render(0);

    const int x = cell_x(0);
    const int y = TEXT_GLYPH_DY;

    // Левый столбец глифа: все семь строк.
    for (int gy = 0; gy < TEXT_GLYPH_H; ++gy) {
        TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + 0, y + gy));
    }

    // Верхняя строка: только левый пиксель, правые - фон.
    for (int gx = 1; gx < TEXT_GLYPH_W; ++gx) {
        TEST_ASSERT_EQUAL_HEX16(BG, pixel(x + gx, y + 0));
    }

    // Нижняя строка: закрашены все пять - это подчёркивание в 'L'.
    for (int gx = 0; gx < TEXT_GLYPH_W; ++gx) {
        TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + gx, y + TEXT_GLYPH_H - 1));
    }
}

// Символ во второй клетке должен попасть во вторую, а не в первую: проверка
// арифметики смещения, где легко забыть поле или ширину клетки.
//
// Берём '|' - у него закрашен ровно средний столбец глифа. Тогда видно и
// столбец внутри клетки (3-й из 5), и границы клетки: если смещение собьётся
// на пиксель, точка уедет из среднего столбца, а не «слегка сдвинется».
static void test_second_cell_offset(void)
{
    text_putc(' ');
    text_putc('|');
    render(0);

    const int x = cell_x(1) + 2;
    for (int gy = 0; gy < TEXT_GLYPH_H; ++gy) {
        TEST_ASSERT_EQUAL_HEX16(FG, pixel(x, TEXT_GLYPH_DY + gy));
    }

    // В первой клетке пусто - иначе символ нарисовался бы дважды.
    TEST_ASSERT_EQUAL_HEX16(BG, pixel(cell_x(0) + 2, TEXT_GLYPH_DY));
}

// Поля по краям закрашиваются фоном, а не остаются от прошлого кадра.
static void test_margins_filled_with_background(void)
{
    text_putc('#');
    render(0);

    for (int y = 0; y < TEXT_CELL_H; ++y) {
        for (int x = 0; x < TEXT_MARGIN_X; ++x) {
            TEST_ASSERT_EQUAL_HEX16(BG, pixel(x, y));
        }
        for (int x = TEXT_STRIP_W - TEXT_MARGIN_X; x < TEXT_STRIP_W; ++x) {
            TEST_ASSERT_EQUAL_HEX16(BG, pixel(x, y));
        }
    }
}

// Курсор рисуется только в своей строке. Проверка не лишняя: полоса строки
// рисуется независимо, и если подчёркивание считать «всегда», оно появляется
// в каждой перерисованной строке - при наборе текста экран покрывается
// чёрточками под первым символом каждой строки.
static void test_cursor_only_on_its_row(void)
{
    text_puts("ab\n");  // курсор уехал на строку 1
    text_cursor_blink(true);

    render(0);
    for (int x = 0; x < TEXT_STRIP_W; ++x) {
        TEST_ASSERT_EQUAL_HEX16(BG, pixel(x, TEXT_CELL_H - 1));
    }

    render(1);
    const int x = cell_x(0);
    TEST_ASSERT_EQUAL_HEX16(FG, pixel(x, TEXT_CELL_H - 1));
}

// --- кириллица и UTF-8 ---

// Два байта UTF-8 - один символ и одна клетка. Если бы клетка считалась на
// байт, «А» заняла бы две, и весь текст после неё поехал бы вправо.
static void test_cyrillic_takes_one_cell(void)
{
    text_puts("\xD0\x90");  // «А»
    TEST_ASSERT_EQUAL_INT(1, text_cursor_col());
}

// Буква доехала до клетки и нарисовалась: у «А» верхний пиксель средней
// колонки и вся средняя строка — поперечина.
static void test_cyrillic_glyph_drawn(void)
{
    text_puts("\xD0\x90");
    render(0);

    const int x = cell_x(0);
    TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + 2, TEXT_GLYPH_DY + 0));
    for (int gx = 0; gx < TEXT_GLYPH_W; ++gx) {
        TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + gx, TEXT_GLYPH_DY + 4));
    }
    // В нижней строке ноги по краям, в середине пусто - иначе это была бы не
    // «А», а сплошной прямоугольник.
    TEST_ASSERT_EQUAL_HEX16(BG, pixel(x + 2, TEXT_GLYPH_DY + 6));
}

// Символ без глифа (эмодзи) рисуется как '?'. Пустая клетка выглядела бы как
// пропавшая буква, и было бы не понять, где кончается шрифт.
static void test_unknown_glyph_becomes_question(void)
{
    text_puts("\xF0\x9F\x98\x80");  // U+1F600
    TEST_ASSERT_EQUAL_INT(1, text_cursor_col());

    render(0);
    const int x = cell_x(0);
    // У '?' верхняя строка — «.###.», то есть средние три пикселя.
    TEST_ASSERT_EQUAL_HEX16(BG, pixel(x + 0, TEXT_GLYPH_DY));
    TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + 1, TEXT_GLYPH_DY));
    TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + 3, TEXT_GLYPH_DY));
}

// Битый байт не двигает курсор и не стирает соседей: следующий нормальный
// символ встаёт на его место.
static void test_invalid_byte_takes_no_cell(void)
{
    text_putc((char)0xB0);  // продолжение без головки
    TEST_ASSERT_EQUAL_INT(0, text_cursor_col());

    text_putc('A');
    TEST_ASSERT_EQUAL_INT(1, text_cursor_col());
}

// Управляющий байт обрывает недособранную букву: иначе хвост следующей
// последовательности склеился бы со старой головкой, и на экране появилась бы
// буква из воздуха.
static void test_control_byte_drops_pending(void)
{
    text_putc((char)0xD0);   // головка «А», ждём продолжение
    text_putc('\n');         // и вдруг перевод строки
    text_putc((char)0x90);   // хвост уже некуда - это не буква

    TEST_ASSERT_EQUAL_INT(0, text_cursor_col());
    TEST_ASSERT_EQUAL_INT(1, text_cursor_row());
}

// Заглавная и строчная кириллица должны различаться формой, а не совпадать:
// именно тут опечатка в генераторе проходит незамеченной.
static void test_cyrillic_upper_and_lower_differ(void)
{
    text_puts("\xD0\x90");  // «А»
    uint16_t upper[TEXT_STRIP_PIXELS];
    render(0);
    memcpy(upper, s_strip, sizeof(upper));

    text_init();
    text_set_colors(FG, BG);
    clean_all();

    text_puts("\xD0\xB0");  // «а»
    render(0);

    TEST_ASSERT_TRUE(memcmp(upper, s_strip, sizeof(upper)) != 0);
}

// --- dirty-строки ---

static void test_single_char_marks_one_row(void)
{
    text_putc('A');
    TEST_ASSERT_EQUAL_INT(1, count_dirty());
    TEST_ASSERT_TRUE(text_row_dirty(0));
}

static void test_clean_clears_flag(void)
{
    text_putc('A');
    text_row_clean(0);
    TEST_ASSERT_FALSE(text_row_dirty(0));
}

// Смена цвета перерисовывает всё: иначе на экране осталась бы смесь.
static void test_color_change_marks_all_rows(void)
{
    clean_all();
    text_set_colors(BG, FG);
    TEST_ASSERT_EQUAL_INT(TEXT_ROWS, count_dirty());
}

// Курсор: подчёркивание в двух нижних строках клетки, глиф под ним цел.
static void test_cursor_draws_underline(void)
{
    text_cursor_at(0, 0);
    text_putc('H');
    text_cursor_at(0, 0);
    text_cursor_blink(true);
    render(0);

    const int x = cell_x(0);
    for (int dx = 0; dx < TEXT_CELL_W; ++dx) {
        TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + dx, TEXT_CELL_H - 1));
        TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + dx, TEXT_CELL_H - 2));
    }

    // Буква под курсором не затёрта: верхняя строка 'H' - два пикселя по краям.
    TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + 0, TEXT_GLYPH_DY));
    TEST_ASSERT_EQUAL_HEX16(FG, pixel(x + TEXT_GLYPH_W - 1, TEXT_GLYPH_DY));
}

static void test_cursor_blink_marks_row(void)
{
    text_cursor_at(3, 5);
    clean_all();

    text_cursor_blink(true);
    TEST_ASSERT_TRUE(text_row_dirty(5));

    // Повторный вызов с тем же значением ничего не меняет: мигание не должно
    // перерисовывать строку на каждом тике, если состояние не изменилось.
    clean_all();
    text_cursor_blink(true);
    TEST_ASSERT_FALSE(text_row_dirty(5));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_geometry_fits_panel);

    RUN_TEST(test_putc_advances_cursor);
    RUN_TEST(test_newline_resets_column);
    RUN_TEST(test_carriage_return_only_moves_column);
    RUN_TEST(test_wrap_after_last_column);
    RUN_TEST(test_scroll_on_last_row);
    RUN_TEST(test_scroll_moves_content_up);
    RUN_TEST(test_unprintable_ignored);
    RUN_TEST(test_tab_to_next_stop);
    RUN_TEST(test_backspace_stops_at_line_start);

    RUN_TEST(test_space_draws_nothing);
    RUN_TEST(test_glyph_bit_order);
    RUN_TEST(test_margins_filled_with_background);

    RUN_TEST(test_cursor_only_on_its_row);

    RUN_TEST(test_cyrillic_takes_one_cell);
    RUN_TEST(test_cyrillic_glyph_drawn);
    RUN_TEST(test_unknown_glyph_becomes_question);
    RUN_TEST(test_invalid_byte_takes_no_cell);
    RUN_TEST(test_control_byte_drops_pending);
    RUN_TEST(test_cyrillic_upper_and_lower_differ);

    RUN_TEST(test_single_char_marks_one_row);
    RUN_TEST(test_clean_clears_flag);
    RUN_TEST(test_color_change_marks_all_rows);
    RUN_TEST(test_cursor_draws_underline);
    RUN_TEST(test_cursor_blink_marks_row);

    return UNITY_END();
}
