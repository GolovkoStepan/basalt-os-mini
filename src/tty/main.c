// env:tty - ввод с мака на экран: A2 своими руками.
//
// Клавиатуры на плате нет, поэтому «набирать» - это печатать в терминале на
// маке, подключённом к нативному USB. Байты приходят в USB Serial/JTAG, идут
// через line discipline (tty/tty.c) и превращаются в события: символ - на
// экран, строка - в разбор команд. Здесь только связка: tty ничего не знает
// про экран и USB, а этот файл - про буфер строки.
//
// Почему приложение, а не сразу шелл: шелл (A3) - это уже про команды и файлы,
// и отлаживать его на непроверенном вводе значит искать причину в двух местах
// сразу. Здесь проверяется ровно ввод: эхо, Backspace по кириллице, raw-режим,
// зеркало в USB.
//
// Два вывода одного потока. Экран - это дисплей, USB-терминал на маке - второй
// дисплей того же tty. Поэтому зеркало дублирует не только набранное, а всё,
// что tty выводит: иначе ответ на команду был бы виден только на экранчике, и
// проверять работу пришлось бы глазами вместо лога.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_pins.h"
#include "button.h"
#include "console_usb_jtag.h"
#include "format.h"
#include "lcd.h"
#include "matrix.h"
#include "start.h"
#include "tick.h"
#include "tty.h"
#include "utf8.h"
#include "vectors.h"

// Полоса одной строки 320x10 в SRAM. Кадровый буфер всего экрана здесь не
// нужен и вреден: см. комментарий в src/text/main.c.
static uint16_t s_strip[TEXT_STRIP_PIXELS];

// Дублировать поток tty в USB. Включается и выключается командой: это
// единственный способ проверить, что зеркало управляемо, а не работает всегда.
static bool s_mirror = true;

// Ввод закрыт по Ctrl-D. Читать порт продолжаем (иначе непонятно, жива ли
// плата), но в tty байты не отдаём: конец ввода должен быть концом, а не
// «следующая буква начинает новую сессию».
static bool s_halt;

// --- вывод: экран и USB ---------------------------------------------------

// Отдать драйверу все изменённые строки. Возвращает число переданных: по нему
// в логе видно, что печать буквы - это одна строка, а не семнадцать.
static int present_dirty(void)
{
    int sent = 0;
    for (int row = 0; row < TEXT_ROWS; ++row) {
        if (!text_row_dirty(row)) {
            continue;
        }

        text_render_row(row, s_strip);
        lcd_flush(0, row * TEXT_CELL_H, TEXT_STRIP_W, TEXT_CELL_H, s_strip);
        text_row_clean(row);
        ++sent;
    }
    return sent;
}

// Вывести строку в USB. Перевод строки превращается в \r\n: терминал мака сам
// курсор на начало не возвращает, и \n без \r оставил бы лесенку из строк.
// Посимвольно, потому что в строке встречается и то, и другое.
static void mirror_stream(const char* s)
{
    if (!s_mirror) {
        return;
    }

    for (const char* p = s; *p != '\0'; ++p) {
        if (*p == '\n') {
            basalt_console_write("\r\n", 2);
        } else {
            basalt_console_write(p, 1);
        }
    }
}

static void mirror_str(const char* s)
{
    mirror_stream(s);
}

// Символ пришёл кодом, а в поток уходит байтами. Кодировщик общий с будущим
// выводом программ - своя арифметика сдвигов разошлась бы с ним на первом же
// трёхбайтном символе.
static void mirror_cp(uint32_t cp)
{
    if (!s_mirror) {
        return;
    }
    if (cp == '\n') {
        // Через кодировщик вышел бы один \n, а терминалу нужно два байта.
        mirror_str("\n");
        return;
    }

    char buf[4];
    const size_t n = utf8_encode(cp, buf);
    basalt_console_write(buf, n);
}

// Символ уже собран декодером, поэтому ASCII идёт через text_putc (он понимает
// \n, \r, \b и \t - в raw-режиме управляющие байты приходят как данные), а
// остальное - через text_put_cp, чтобы не гонять код обратно в байты.
static void screen_put_cp(uint32_t cp)
{
    if (cp < 0x80u) {
        text_putc((char)cp);
    } else {
        text_put_cp(cp);
    }
}

// Строка - на оба вывода. Одна функция на всё, что печатает приложение: если
// где-то писать только на экран, зеркало перестанет быть зеркалом, и это
// заметят не сразу, а при разборе лога через неделю.
static void out_puts(const char* s)
{
    if (s == NULL) {
        return;
    }
    text_puts(s);
    mirror_stream(s);
}

static void out_cp(uint32_t cp)
{
    screen_put_cp(cp);
    mirror_cp(cp);
}

static void out_u64(uint64_t v)
{
    char buf[21];
    out_puts(basalt_format_u64(buf, sizeof(buf), v));
}

// --- команды --------------------------------------------------------------

static void print_help(void)
{
    out_puts("команды:\n");
    out_puts("  help           этот список\n");
    out_puts("  mirror on|off  повторять поток в USB\n");
    out_puts("  raw / canon    режим tty\n");
    out_puts("  clear          очистить экран\n");
    out_puts("  uptime         время работы платы\n");
    out_puts("G0 (BOOT) - Ctrl-C, если терминал\n");
    out_puts("съедает ^C сам.\n");
}

// Разбор строки. Строка уже отдана tty и годна до следующего байта - поэтому
// здесь её можно читать, но нельзя запоминать.
static void run_command(const char* line)
{
    if (line[0] == '\0') {
        return;
    }

    if (strcmp(line, "help") == 0) {
        print_help();
        return;
    }

    if (strcmp(line, "mirror on") == 0) {
        // Включаем до ответа: иначе подтверждение не дошло бы до терминала, и
        // было бы не понять, сработала ли команда.
        s_mirror = true;
        out_puts("зеркало включено\n");
        return;
    }

    if (strcmp(line, "mirror off") == 0) {
        out_puts("зеркало выключено\n");
        s_mirror = false;
        return;
    }

    if (strcmp(line, "mirror") == 0) {
        out_puts(s_mirror ? "зеркало включено\n" : "зеркало выключено\n");
        return;
    }

    if (strcmp(line, "raw") == 0) {
        tty_set_mode(TTY_MODE_RAW);
        s_mirror = false;
        out_puts("raw: символы идут как есть, Ctrl-C - выход\n");
        return;
    }

    if (strcmp(line, "canon") == 0) {
        tty_set_mode(TTY_MODE_CANON);
        out_puts("canon: построчный ввод, эхо\n");
        return;
    }

    if (strcmp(line, "clear") == 0) {
        text_clear();
        out_puts("basalt-os tty, экран очищен\n");
        return;
    }

    if (strcmp(line, "uptime") == 0) {
        out_puts("аптайм ");
        out_u64(basalt_uptime_us() / 1000000u);
        out_puts(" с\n");
        return;
    }

    out_puts("?: ");
    out_puts(line);
    out_puts("\nhelp - список команд\n");
}

// --- обработка событий tty ------------------------------------------------

static void handle(const tty_result r)
{
    switch (r.event) {
    case TTY_EV_NONE:
        break;

    case TTY_EV_CHAR:
        // Символ рисуется всегда: экран - это дисплей, а не эхо. В raw-режиме
        // зеркало выключено, поэтому там символы видны только на экране - это
        // и есть «raw без эха».
        out_cp(r.cp);
        break;

    case TTY_EV_ERASE:
        // Стираем одну клетку: \b отводит курсор, пробел затирает, второй \b
        // возвращает его на место. Так же делает настоящий терминал, и по
        // одному движению курсора это не отличить.
        //
        // Начало строки - единственный случай, когда стереть нечего: матрица
        // не переводит \b на предыдущую строку намеренно (см. matrix.c),
        // поэтому клетка остаётся. Сюда попадаешь, только если строка была
        // ровно в 53 символа и перенос её уже прокрутил.
        if (text_cursor_col() > 0) {
            text_putc('\b');
            text_putc(' ');
            text_putc('\b');
        }
        // На экране для этого три действия, в потоке - те же три байта, что
        // присылает настоящий терминал при стирании.
        mirror_str("\b \b");
        break;

    case TTY_EV_LINE:
        // Строка на экране уже набрана по символам - здесь только конец строки
        // и разбор.
        out_cp('\n');
        run_command(tty_line());
        break;

    case TTY_EV_INTR: {
        // Из raw надо выйти: иначе Ctrl-C прерывал бы программу, которую
        // нечем остановить, и вернуться в канон было бы нечем. Режим и зеркало
        // возвращаем ДО печати, чтобы ^C было видно в обоих выводах: иначе
        // в USB пришло бы только сообщение о переходе, а самого прерывания нет.
        const bool was_raw = (tty_mode_get() == TTY_MODE_RAW);
        if (was_raw) {
            tty_set_mode(TTY_MODE_CANON);
            s_mirror = true;
        }

        // ^C печатается, чтобы прерывание было видно в потоке: иначе строка
        // просто исчезнет, и это будет выглядеть как сбой.
        out_puts("^C\n");
        if (was_raw) {
            out_puts("обратно в canon, зеркало включено\n");
        }
        break;
    }

    case TTY_EV_EOF:
        s_halt = true;
        out_puts("^D конец ввода, RST - заново\n");
        break;

    case TTY_EV_BELL:
        // Стирать некуда. На экране это заметка, а в поток - \a: у платы нет
        // пищалки, и «ничего не произошло» было бы не отличить от «стёрлось».
        // Сначала звук, потом заметка: терминал мака пищит в момент нажатия,
        // а не после строки текста.
        mirror_str("\a");
        out_puts("^G\n");
        break;
    }
}

// Кнопка G0 как Ctrl-C. Терминал на маке часто перехватывает ^C сам (посылает
// сигнал своему процессу), и тогда байт 0x03 до платы не доходит вовсе.
// Кнопка гарантирует выход из raw-режима без перепрошивки.
//
// Фронт, а не уровень: удержание не должно слать ^C каждый проход цикла.
static bool s_g0_down;
static uint64_t s_g0_change_us = 20000u;  // заведомо больше порога на старте

static void poll_button(void)
{
    const bool down = button_down(BUTTON_PRIMARY);
    if (down == s_g0_down) {
        return;
    }

    const uint64_t now = basalt_uptime_us();
    // Дребезг механической кнопки - единицы миллисекунд. Порог 20 мс: короче
    // дребезг, длиннее человеческое нажатие.
    if (now - s_g0_change_us < 20000u) {
        return;
    }
    s_g0_change_us = now;
    s_g0_down = down;

    if (down && !s_halt) {
        handle(tty_input(0x03));
    }
}

// --- старт ----------------------------------------------------------------

static void draw_header(void)
{
    out_puts("basalt-os tty\n");
    out_puts("печатайте на маке - текст идёт на экран\n");
    out_puts("\n");
    print_help();
    out_puts("\n");
}

void basalt_main(void)
{
    // Порядок: консоль первая; экран до паузы, чтобы на нём не висел кадр
    // прошлой сессии (панель хранит его в своей памяти, пока не затрут);
    // векторы после паузы - их отчёт печатается, а хост ещё не открыл порт.
    // Подробнее - в src/shell/main.c.
    basalt_console_init();
    basalt_tick_init();

    lcd_init();
    lcd_backlight(true);
    text_init();
    present_dirty();

    basalt_delay_us(2000000);
    basalt_vectors_init();

    const uint64_t t_boot = basalt_uptime_us();

    basalt_put("\n\nbasalt-os tty\n");

    tty_init();
    button_init();

    draw_header();

    basalt_put("[tty] экран ");
    basalt_put_u64(TEXT_COLS);
    basalt_put("x");
    basalt_put_u64(TEXT_ROWS);
    basalt_put(", режим canon, зеркало вкл\n");
    basalt_put("[tty] команды: help, mirror on|off, raw, canon, clear, uptime\n");
    basalt_put("[tty] G0 = Ctrl-C\n");
    basalt_put("ready\n\n");

    uint64_t next_blink = t_boot + 500000u;
    bool blink = true;

    for (;;) {
        // Приняли всё, что есть, и только потом рисуем: при вставке строки из
        // буфера обмена это одна передача строки, а не по одной на символ.
        bool activity = false;
        if (!s_halt) {
            uint8_t buf[64];
            const size_t got = basalt_console_read(buf, sizeof(buf));
            for (size_t i = 0; i < got; ++i) {
                handle(tty_input(buf[i]));
            }
            activity = (got > 0);
        }

        poll_button();

        const uint64_t now = basalt_uptime_us();
        if (activity) {
            // Нажатие показывает курсор сразу и сдвигает фазу мигания. Иначе
            // буква встаёт, а подчёркивания под ней нет до полусекунды, и
            // кажется, что ввод не дошёл до платы.
            blink = true;
            text_cursor_blink(true);
            next_blink = now + 500000u;
        } else if (now >= next_blink) {
            next_blink += 500000u;
            blink = !blink;
            text_cursor_blink(blink);
        }

        present_dirty();

        // Полмиллисекунды: реже - заметно на глаз при наборе, чаще - смысла нет,
        // передача строки всё равно стоит около двух миллисекунд.
        basalt_delay_us(500);
    }
}
