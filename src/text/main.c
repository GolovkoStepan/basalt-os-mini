// env:text - текстовая матрица на живом экране.
//
// Задача вечера: A1 закрыт, когда на экране стоит текст, курсор мигает,
// прокрутка идёт без мусора, а время строки и полного кадра измерены. Поэтому
// здесь показан не «hello» одной строкой, а весь набор глифов, ровно 53
// символа в строке (граница переноса) и строка, которая обновляется раз в
// секунду - по ней видно, что dirty-строки работают и остальной экран не
// перерисовывается.
//
// Этот main.c - щуп вехи, а не система: настоящий вывод пойдёт через tty
// (веха A2), и тогда text_putc будет звать не цикл, а консоль.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console_usb_jtag.h"
#include "format.h"
#include "lcd.h"
#include "matrix.h"
#include "start.h"
#include "tick.h"
#include "vectors.h"

// Полоса одной строки, 320x10 пикселей, в SRAM: 6400 Б. Кадровый буфер на весь
// экран здесь не нужен намеренно - он в PSRAM, а писать в него пиксель за
// пикселем дороже, чем нарисовать одну строку и отдать её драйверу.
static uint16_t s_strip[TEXT_STRIP_PIXELS];

static uint64_t s_last_row_us;

// Число в матрицу. Через тот же формат, что и консоль: иначе через месяц
// число на экране и число в логе разойдутся, и искать причину будут в драйвере.
static void screen_put_u64(uint64_t v)
{
    char buf[21];
    text_puts(basalt_format_u64(buf, sizeof(buf), v));
}

// Отдать драйверу все измененные строки. Возвращает число переданных строк:
// по нему в логе видно, что перерисовывается одна строка, а не семнадцать.
static int present_dirty(void)
{
    int sent = 0;
    for (int row = 0; row < TEXT_ROWS; ++row) {
        if (!text_row_dirty(row)) {
            continue;
        }

        text_render_row(row, s_strip);

        const uint64_t start = basalt_uptime_us();
        lcd_flush(0, row * TEXT_CELL_H, TEXT_STRIP_W, TEXT_CELL_H, s_strip);
        s_last_row_us = basalt_uptime_us() - start;

        text_row_clean(row);
        ++sent;
    }
    return sent;
}

// Перерисовать всё независимо от флагов - нужно для замера полного кадра и
// после того, как экран погасили или переинициализировали.
static uint64_t present_all(void)
{
    const uint64_t start = basalt_uptime_us();
    for (int row = 0; row < TEXT_ROWS; ++row) {
        text_render_row(row, s_strip);

        const uint64_t row_start = basalt_uptime_us();
        lcd_flush(0, row * TEXT_CELL_H, TEXT_STRIP_W, TEXT_CELL_H, s_strip);
        s_last_row_us = basalt_uptime_us() - row_start;

        text_row_clean(row);
    }
    return basalt_uptime_us() - start;
}

// Строка из ровно TEXT_COLS символов. Смысл не в узоре, а в том, что переноса
// быть не должно: если бы последняя клетка уезжала на следующую строку,
// узор поплыл бы, и это было бы видно.
static void put_exact_line(void)
{
    static const char kDigits[] = "0123456789";
    for (int i = 0; i < TEXT_COLS; ++i) {
        text_putc(kDigits[i % 10]);
    }
    text_putc('\n');
}

// Шестнадцать строк: последняя из них - строка 15, а в 16-й живёт строка
// состояния. Если статики окажется больше, её хвост прокрутит экран при
// первой же печати статуса, и часть текста уедет вверх.
static void draw_static_content(void)
{
    text_puts("hello, basalt-os\n");
    text_puts("привет, базальт-ос!\n");
    text_putc('\n');

    text_puts("ABCDEFGHIJKLMNOPQRSTUVWXYZ\n");
    text_puts("abcdefghijklmnopqrstuvwxyz\n");
    text_puts("0123456789 .,:;!?+-/%()[]<>*#@'\"\n");
    text_puts("=~^_|{}\\\n");
    text_putc('\n');

    // Весь алфавит по 33 буквы в строку: 33 клетки из 53 помещаются с запасом.
    // Печатается целиком, потому что форма редких букв (Ж, Щ, Ю, Ы, Ъ) видна
    // только здесь, а в обычном тексте они попадаются раз в абзац.
    text_puts("АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ\n");
    text_puts("абвгдеёжзийклмнопрстуфхцчшщъыьэюя\n");
    text_putc('\n');

    text_puts("53 колонки, 17 строк, клетка 6x10\n");
    put_exact_line();
    text_puts("^ ровно 53 символа, выше переноса нет\n");
    text_putc('\n');

    text_puts("экран 320x170, шрифт 5x7 из make_font.py\n");
}

void basalt_main(void)
{
    // Порядок как в остальных env: консоль первой - если что-то ниже зависнет,
    // след останется в логе; пауза до вывода, чтобы хост открыл порт; векторы
    // до работы с периферией.
    basalt_console_init();
    basalt_tick_init();
    basalt_delay_us(2000000);
    basalt_vectors_init();

    const uint64_t t_boot = basalt_uptime_us();

    basalt_put("\n\nbasalt-os text\n");

    // Подсветка включается до первой заливки: иначе на тёмном экране не видно,
    // случилось ли что-нибудь вообще.
    lcd_init();
    lcd_backlight(true);
    text_init();

    draw_static_content();

    // Полный кадр - это семнадцать строк подряд. Меряем сразу после того, как
    // строки заполнены, и печатаем в консоль: числа нужны в docs/board.md.
    const uint64_t frame_us = present_all();
    basalt_put("[text] матрица ");
    basalt_put_u64(TEXT_COLS);
    basalt_put("x");
    basalt_put_u64(TEXT_ROWS);
    basalt_put(", клетка ");
    basalt_put_u64(TEXT_CELL_W);
    basalt_put("x");
    basalt_put_u64(TEXT_CELL_H);
    basalt_put(", глиф ");
    basalt_put_u64(TEXT_GLYPH_W);
    basalt_put("x");
    basalt_put_u64(TEXT_GLYPH_H);
    basalt_put("\n[text] полный кадр: 17 строк за ");
    basalt_put_u64(frame_us);
    basalt_put(" мкс\n");
    basalt_put("[text] строка 320x10 за ");
    basalt_put_u64(s_last_row_us);
    basalt_put(" мкс\n");

    basalt_put("[text] на экране: весь ASCII и кириллица, ровно 53 символа в строке\n");
    basalt_put("ready\n\n");

    // Строка состояния внизу. Она перерисовывается раз в секунду, и в лог
    // печатается число переданных строк - единица означает, что dirty-строки
    // работают, семнадцать означало бы полную перерисовку на каждый тик.
    uint32_t tick = 0;
    uint64_t next_tick = t_boot + 1000000u;
    uint64_t next_blink = t_boot + 500000u;
    bool blink = true;

    for (;;) {
        const uint64_t now = basalt_uptime_us();

        if (now >= next_tick) {
            next_tick += 1000000u;
            ++tick;

            // Строка состояния собирается заново на каждой тике. Перевод
            // строки в конце НЕ ставится: курсор стоит в последней строке
            // экрана, и \n от неё означал бы прокрутку на каждом тике.
            // Остаток строки добивается пробелами - иначе хвост длинного
            // числа остался бы от прошлого раза.
            text_cursor_at(0, TEXT_ROWS - 1);
            text_puts("аптайм ");
            screen_put_u64(now / 1000000u);
            text_puts(" с, строка ");
            screen_put_u64(s_last_row_us);
            text_puts(" мкс, тик ");
            screen_put_u64(tick);
            while (text_cursor_col() < TEXT_COLS - 1) {
                text_putc(' ');
            }

            const int sent = present_dirty();
            basalt_put("tick ");
            basalt_put_u64(tick);
            basalt_put("  строк передано ");
            basalt_put_u64((uint64_t)sent);
            basalt_put(", строка ");
            basalt_put_u64(s_last_row_us);
            basalt_put(" мкс\n");
        }

        // Мигание курсора: одна строка, раз в полсекунды. Тик для этого не
        // годится - секунда мигания выглядит как «курсор пропал».
        if (now >= next_blink) {
            next_blink += 500000u;
            blink = !blink;
            text_cursor_at(TEXT_COLS - 1, TEXT_ROWS - 1);
            text_cursor_blink(blink);
            present_dirty();
        }

        basalt_delay_us(1000);
    }
}
