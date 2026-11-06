// Текстовая матрица. Зачем отдельный слой - см. matrix.h.

#include "matrix.h"

#include <stddef.h>
#include <string.h>

#include "font_5x7.h"

// Клетки символов. Один байт на клетку, а не пиксели: экран 320x170 - это
// 108 800 Б кадра, а сетка символов - 901 Б. Перерисовывается в пиксели
// только то, что помечено изменённым.
static char s_cells[TEXT_ROWS][TEXT_COLS];

static int s_col;
static int s_row;

static bool s_dirty[TEXT_ROWS];
static bool s_cursor;

// Белый на чёрном. Не «красиво», а безопасно: это единственная пара, которая
// одинаково читается на любой панели и при любой инверсии цветов.
static uint16_t s_fg = 0xFFFF;
static uint16_t s_bg = 0x0000;

static void mark_dirty(int row)
{
    if (row >= 0 && row < TEXT_ROWS) {
        s_dirty[row] = true;
    }
}

static void clear_cells(void)
{
    memset(s_cells, ' ', sizeof(s_cells));
    s_col = 0;
    s_row = 0;
    s_cursor = false;

    for (int r = 0; r < TEXT_ROWS; ++r) {
        s_dirty[r] = true;
    }
}

void text_init(void)
{
    clear_cells();
}

void text_clear(void)
{
    clear_cells();
}

void text_set_colors(uint16_t fg, uint16_t bg)
{
    if (fg == s_fg && bg == s_bg) {
        return;
    }
    s_fg = fg;
    s_bg = bg;
    for (int r = 0; r < TEXT_ROWS; ++r) {
        s_dirty[r] = true;
    }
}

// Прокрутка на строку вверх. Прокручивается сетка символов (901 Б), а не
// пиксели: пиксели всё равно придётся рисовать заново целиком, так дешевле.
static void scroll_up(void)
{
    for (int r = 1; r < TEXT_ROWS; ++r) {
        memcpy(s_cells[r - 1], s_cells[r], TEXT_COLS);
    }
    memset(s_cells[TEXT_ROWS - 1], ' ', TEXT_COLS);

    for (int r = 0; r < TEXT_ROWS; ++r) {
        s_dirty[r] = true;
    }
    s_row = TEXT_ROWS - 1;
}

static void newline(void)
{
    const int old_row = s_row;
    s_col = 0;
    ++s_row;
    mark_dirty(old_row);

    if (s_row >= TEXT_ROWS) {
        scroll_up();
    }
    mark_dirty(s_row);
}

static void put_byte(char c)
{
    // Вне печатного диапазона шрифта нет. Пропускаем молча, а не рисуем
    // заглушку: заглушка на каждом байте испортила бы вывод UTF-8, который
    // придёт сюда, когда появится кириллица, - и было бы не понять, где
    // кончается шрифт, а где начинается приём.
    const unsigned char u = (unsigned char)c;
    if (u < 0x20 || u > 0x7E) {
        return;
    }

    s_cells[s_row][s_col] = c;
    mark_dirty(s_row);

    ++s_col;
    if (s_col >= TEXT_COLS) {
        newline();
    }
}

void text_putc(char c)
{
    switch (c) {
    case '\n':
        newline();
        break;

    case '\r':
        s_col = 0;
        break;

    case '\b':
        // В начало предыдущей строки не переходим: на экране это выглядело бы
        // как «стёрся конец строки и появился её же конец выше», а настоящий
        // терминал так не делает без явного возврата каретки.
        if (s_col > 0) {
            --s_col;
            mark_dirty(s_row);
        }
        break;

    case '\t':
        // До ближайшей позиции, кратной восьми. Цикл, а не арифметика: вывод
        // символа идёт через ту же дорогу, что и всё остальное, включая
        // возможный перенос.
        do {
            put_byte(' ');
        } while (s_col % 8 != 0);
        break;

    default:
        put_byte(c);
        break;
    }

    // Курсор мог уехать на другую строку любым из путей выше.
    mark_dirty(s_row);
}

void text_puts(const char* s)
{
    for (const char* p = s; *p != '\0'; ++p) {
        text_putc(*p);
    }
}

void text_cursor_at(int col, int row)
{
    if (col < 0) {
        col = 0;
    }
    if (col >= TEXT_COLS) {
        col = TEXT_COLS - 1;
    }
    if (row < 0) {
        row = 0;
    }
    if (row >= TEXT_ROWS) {
        row = TEXT_ROWS - 1;
    }

    // Курсор исчез со старой клетки - её тоже надо перерисовать.
    mark_dirty(s_row);

    s_col = col;
    s_row = row;
    mark_dirty(s_row);
}

int text_cursor_col(void)
{
    return s_col;
}

int text_cursor_row(void)
{
    return s_row;
}

void text_cursor_blink(bool visible)
{
    if (visible == s_cursor) {
        return;
    }
    s_cursor = visible;
    mark_dirty(s_row);
}

bool text_cursor_visible(void)
{
    return s_cursor;
}

bool text_row_dirty(int row)
{
    if (row < 0 || row >= TEXT_ROWS) {
        return false;
    }
    return s_dirty[row];
}

void text_row_clean(int row)
{
    if (row >= 0 && row < TEXT_ROWS) {
        s_dirty[row] = false;
    }
}

void text_render_row(int row, uint16_t* dst)
{
    if (dst == NULL || row < 0 || row >= TEXT_ROWS) {
        return;
    }

    // Сначала фон на всю полосу, включая поля по краям: так поля всегда
    // совпадают с фоном текста, и о них не приходится помнить вызывающему.
    for (int i = 0; i < TEXT_STRIP_PIXELS; ++i) {
        dst[i] = s_bg;
    }

    // Только точки глифов поверх фона. Клетка не обводится: у ST7789 нет
    // обратного чтения, и стереть неверно нарисованное можно только заливкой
    // фона, то есть тем же, что уже сделано выше.
    for (int col = 0; col < TEXT_COLS; ++col) {
        const unsigned char ch = (unsigned char)s_cells[row][col];
        if (ch < FONT_FIRST_CODE || ch >= FONT_FIRST_CODE + FONT_GLYPH_COUNT) {
            continue;
        }

        const uint8_t* glyph = kFont5x7[ch - FONT_FIRST_CODE];
        const int base_x = TEXT_MARGIN_X + col * TEXT_CELL_W;

        for (int gy = 0; gy < TEXT_GLYPH_H; ++gy) {
            const uint8_t bits = glyph[gy];
            if (bits == 0) {
                continue;
            }

            uint16_t* line = dst + (size_t)(gy + TEXT_GLYPH_DY) * TEXT_STRIP_W + base_x;

            // Старший бит строки - левый пиксель. Порядок важен: при чтении
            // наоборот буквы выйдут зеркальными, и это заметно не сразу.
            for (int gx = 0; gx < TEXT_GLYPH_W; ++gx) {
                if (bits & (0x10u >> gx)) {
                    line[gx] = s_fg;
                }
            }
        }
    }

    // Курсор - подчёркивание в двух нижних строках клетки. Под буквой, а не
    // поверх неё: так видно и символ под курсором, и то, куда пойдёт
    // следующий.
    if (s_cursor) {
        const int base_x = TEXT_MARGIN_X + s_col * TEXT_CELL_W;
        for (int dy = TEXT_CELL_H - 2; dy < TEXT_CELL_H; ++dy) {
            uint16_t* line = dst + (size_t)dy * TEXT_STRIP_W + base_x;
            for (int dx = 0; dx < TEXT_CELL_W; ++dx) {
                line[dx] = s_fg;
            }
        }
    }
}
