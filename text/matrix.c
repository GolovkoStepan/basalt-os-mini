// Текстовая матрица. Зачем отдельный слой - см. matrix.h.

#include "matrix.h"

#include <stddef.h>
#include <string.h>

#include "font_5x7.h"
#include "utf8.h"


// Клетки символов: по code point на клетку. 17*53*2 = 1802 Б против 108 800 Б
// кадра - сетку дешевле двигать, чем пиксели. Ноль означает пустую клетку и
// заодно служит значением по умолчанию после memset: сам символ с кодом 0
// (NUL) в клетку не попадает - это управляющий байт, а не глиф.
static uint16_t s_cells[TEXT_ROWS][TEXT_COLS];

// Состояние разбора UTF-8 живёт здесь, а не в декодере: недособранная буква
// принадлежит этому экрану. Когда появится второй поток (A2), у него будет
// свой декодер, и символы двух потоков не склеятся.
static utf8_decoder s_utf8;

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
    memset(s_cells, 0, sizeof(s_cells));
    utf8_reset(&s_utf8);
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

// Прокрутка на строку вверх. Прокручивается сетка символов (1802 Б), а не
// пиксели: пиксели всё равно придётся рисовать заново целиком, так дешевле.
static void scroll_up(void)
{
    for (int r = 1; r < TEXT_ROWS; ++r) {
        memcpy(s_cells[r - 1], s_cells[r], sizeof(s_cells[0]));
    }
    memset(s_cells[TEXT_ROWS - 1], 0, sizeof(s_cells[0]));

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

void text_put_cp(uint32_t cp)
{
    // Управляющие коды - команды, а не символы. Сюда они приходят только
    // байтами потока (text_putc разбирает \n, \r, \b и \t сам), и глифа у
    // них нет: молча пропускаем.
    if (cp < 0x20 || cp == 0x7F) {
        return;
    }

    // Символ настоящий, но глифа нет: за пределами ASCII и кириллицы шрифт
    // пуст. Рисуем '?' - пустая клетка выглядела бы как пропавшая буква при
    // живом потоке, и было бы не понять, где кончается шрифт.
    if (font_glyph(cp) == NULL) {
        cp = '?';
    }

    s_cells[s_row][s_col] = (uint16_t)cp;
    mark_dirty(s_row);

    ++s_col;
    if (s_col >= TEXT_COLS) {
        newline();
    }
}

void text_putc(char c)
{
    const uint8_t byte = (uint8_t)c;

    // Управляющий байт означает возврат к командам: недособранная буква после
    // него не продолжится. Держать её состояние опасно - следующий 0xB0
    // склеился бы со старой головкой, и на экране появилась бы буква из
    // воздуха.
    if (byte < 0x20 || byte == 0x7F) {
        utf8_reset(&s_utf8);
    }

    switch (byte) {
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
            text_put_cp(' ');
        } while (s_col % 8 != 0);
        break;

    default:
        if (byte < 0x80) {
            // ASCII мимо декодера, но состояние всё равно сбрасываем: буква
            // посреди недособранной последовательности означает, что та битая.
            utf8_reset(&s_utf8);
            text_put_cp(byte);
        } else {
            uint32_t cp = 0;
            // PENDING - буква ещё не собрана; INVALID - байт выброшен и
            // курсор двигать не за что.
            if (utf8_feed(&s_utf8, byte, &cp) == UTF8_DONE) {
                text_put_cp(cp);
            }
        }
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
        const uint16_t cp = s_cells[row][col];
        if (cp == 0) {
            continue;
        }

        const uint8_t* glyph = font_glyph(cp);
        if (glyph == NULL) {
            // В клетку такое попасть не может: text_put_cp подменяет символ
            // без глифа на '?'. Проверка на случай, если клетку однажды начнут
            // писать в обход text_put_cp.
            continue;
        }
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
