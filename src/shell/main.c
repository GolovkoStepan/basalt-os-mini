// env:shell - шелл: командная строка с редактированием, история, команды.
//
// Что здесь и что не здесь. Разбор строки и правка живут в shell/ и проверяются
// на маке (test_parse, test_lineedit, test_history). Этот файл - связка, которой
// нужна плата: экран, USB, куча, кнопка. Он переводит tty в raw и отдаёт
// символы редактору строки, рисует строку в матрицу и зеркалит поток в USB.
//
// Почему raw, а не канон. В каноне tty сам собирает строку, но умеет только
// «дописать в конец» и «стереть последний». Стрелки, Ctrl-A/E/K/U/W и история
// требуют курсора внутри строки, поэтому строкой владеет редактор - ровно так
// устроены настоящие терминалы. tty при этом остаётся про байты и не знает про
// клавиши.
//
// Ввод идёт через tty, а не напрямую из USB: декодер UTF-8 должен быть один на
// поток. Своя раскатка байтов в символы разошлась бы с tty на первом же
// трёхбайтном символе, и заметили бы это на кириллице.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "button.h"
#include "console_usb_jtag.h"
#include "esp_heap_caps.h"
#include "format.h"
#include "history.h"
#include "lcd.h"
#include "lineedit.h"
#include "matrix.h"
#include "parse.h"
#include "stack.h"
#include "start.h"
#include "tick.h"
#include "tty.h"
#include "utf8.h"
#include "vectors.h"

#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

// Приглашение. Ширину отдельной константой не держим: после его печати
// приложение запоминает колонку курсора, и она же служит началом строки.
#define PROMPT "> "

// Полоса одной строки 320x10 в SRAM. Кадровый буфер всего экрана не нужен:
// вывод идёт построчно, см. src/text/main.c.
static uint16_t s_strip[TEXT_STRIP_PIXELS];

// Дублировать поток в USB. Выключается командой `mirror off` - это по-прежнему
// единственный способ проверить, что зеркало управляемо.
static bool s_mirror = true;

// Ввод закрыт по Ctrl-D: читать порт продолжаем (иначе не видно, жива ли
// плата), но в редактор байты не отдаём.
static bool s_halt;

// --- вывод: экран и USB ---------------------------------------------------

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

// Перевод строки превращается в \r\n: терминал мака сам курсор на начало не
// возвращает, и \n без \r оставил бы лесенку.
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

// Символ пришёл кодом, а в поток уходит байтами. Кодировщик общий с tty и с
// редактором - своя арифметика разошлась бы с ними на первом же трёхбайтном
// символе.
static void mirror_cp(uint32_t cp)
{
    if (!s_mirror) {
        return;
    }
    if (cp == '\n') {
        mirror_str("\n");
        return;
    }

    char buf[4];
    const size_t n = utf8_encode(cp, buf);
    basalt_console_write(buf, n);
}

// ASCII идёт через text_putc (он понимает \n, \r, \b и \t), остальное - через
// text_put_cp: гонять готовый код обратно в байты незачем.
static void screen_put_cp(uint32_t cp)
{
    if (cp < 0x80u) {
        text_putc((char)cp);
    } else {
        text_put_cp(cp);
    }
}

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

// Число двумя знаками: «07», а не «7». Времени иначе не прочитать.
static void out_u64_2(uint64_t v)
{
    if (v < 10) {
        out_puts("0");
    }
    out_u64(v);
}

// Кибибайты. Байты в отчёте о памяти ничего не говорят, а перевод в КБ -
// одна операция, которая не ошибается.
static void out_kib(size_t bytes)
{
    out_u64(bytes / 1024u);
    out_puts(" КБ");
}

// --- строка на экране -----------------------------------------------------

// Где стоит приглашение и строка. Запоминается в момент печати приглашения:
// после вывода команды экран мог прокрутиться, и «предыдущая строка» перестала
// бы совпадать с тем, что на экране.
static int s_line_row;
static int s_line_col;

// Первая показанная клетка строки. Команда длиннее экрана прокручивается
// по горизонтали, как в однострочном поле ввода: рисовать 256 символов в 51
// клетку нельзя, а перенос на следующую строку сломал бы и курсор, и стирание
// остатка.
static size_t s_win;

// Сколько клеток нарисовано с прошлой отрисовки. Нужно, чтобы стереть хвост,
// когда строку сократили.
static size_t s_drawn;

static void prompt(void)
{
    mirror_str("\r");
    out_puts(PROMPT);

    s_line_row = text_cursor_row();
    s_line_col = text_cursor_col();
    s_win = 0;
    s_drawn = 0;
}

// Перерисовать строку целиком: от приглашения до курсора. Дёшево - одна
// строка, и только она помечается изменённой.
static void draw_line(void)
{
    const size_t avail = (size_t)(TEXT_COLS - s_line_col);
    const size_t cols = le_cols();
    const size_t cur = le_cursor_col();

    // Держим курсор в окне: сдвигаем окно только когда он выходит за край,
    // иначе строка «прыгала» бы на каждой букве.
    if (cur < s_win) {
        s_win = cur;
    }
    if (cur >= s_win + avail) {
        s_win = cur - avail + 1;
    }

    text_cursor_at(s_line_col, s_line_row);

    const size_t shown = (cols > s_win) ? (cols - s_win) : 0;
    const size_t n = (shown > avail) ? avail : shown;
    for (size_t i = 0; i < n; ++i) {
        screen_put_cp(le_cell_cp(s_win + i));
    }

    // Хвост прошлой отрисовки затираем пробелами: стереть клетку иначе нечем.
    for (size_t i = n; i < s_drawn; ++i) {
        text_putc(' ');
    }
    s_drawn = n;

    text_cursor_at(s_line_col + (int)(cur - s_win), s_line_row);

    // Зеркало - тоже целиком: терминал мака сам не знает, что мы правим строку.
    // \r в начало, приглашение, строка, \x1b[K - стереть хвост, и обратно на
    // позицию курсора.
    if (s_mirror) {
        mirror_str("\r");
        mirror_str(PROMPT);
        mirror_str(le_line());
        mirror_str("\x1b[K");
        const size_t back = le_cols() - le_cursor_col();
        for (size_t i = 0; i < back; ++i) {
            mirror_str("\b");
        }
    }
}

// --- команды --------------------------------------------------------------

typedef void (*command_fn)(int argc, char** argv);

typedef struct {
    const char* name;
    command_fn fn;
    const char* help;
} command;

static void cmd_help(int argc, char** argv);

static void cmd_echo(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            out_puts(" ");
        }
        out_puts(argv[i]);
    }
    out_puts("\n");
}

static void cmd_history(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const int count = history_count();
    if (count == 0) {
        out_puts("история пуста\n");
        return;
    }

    for (int i = 0; i < count; ++i) {
        out_puts("  ");
        out_u64((uint64_t)(i + 1));
        out_puts("  ");
        out_puts(history_get(i));
        out_puts("\n");
    }
}

// Дата и время. Часов реального времени у платы нет: нет ни RTC-микросхемы,
// ни NTP. Поэтому показываем то, что действительно известно - сколько прошло
// с запуска. Врать календарной датой из ниоткуда было бы хуже, чем её не иметь.
static void cmd_date(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const uint64_t total = basalt_uptime_us() / 1000000u;
    const uint64_t days = total / 86400u;
    const uint64_t rest = total % 86400u;

    out_puts("date: ");
    out_u64(days);
    out_puts(" д ");
    out_u64_2(rest / 3600u);
    out_puts(":");
    out_u64_2((rest % 3600u) / 60u);
    out_puts(":");
    out_u64_2(rest % 60u);
    out_puts(" с запуска\n");
    out_puts("календаря нет: RTC на плате нет, NTP появится вместе с Wi-Fi\n");
}

// Проценты печатает format.c: формула живёт в одном месте, иначе «12%» в `top`
// и «11%» в `free` про одно и то же число.
static void out_percent(size_t part, size_t total)
{
    char buf[8];
    out_puts(basalt_format_percent(buf, sizeof(buf), part, total));
    out_puts("%");
}

static void put_heap_line(const char* name, uint32_t caps)
{
    // Имя дополняем пробелами до одной ширины: колонка чисел должна читаться
    // сверху вниз, иначе сравнивать SRAM с PSRAM приходится по буквам.
    out_puts(name);
    for (size_t pad = strlen(name); pad < 5; ++pad) {
        out_puts(" ");
    }
    out_puts(": ");

    const size_t total = heap_caps_get_total_size(caps);
    if (total == 0) {
        out_puts("нет\n");
        return;
    }

    const size_t free_bytes = heap_caps_get_free_size(caps);

    // «Занято» печатается как разность уже переведённых в КБ чисел, а не как
    // свой перевод: иначе 8192 - 8189 давало бы на экране занято 2 КБ вместо 3,
    // и читатель ловил бы арифметику, которая не складывается. Проценты при
    // этом считаются по байтам - им точность нужнее, чем согласованность строки.
    const size_t total_kib = total / 1024u;
    const size_t free_kib = free_bytes / 1024u;
    const size_t used_kib = total_kib - free_kib;

    // Две строки, а не одна: на экране 53 клетки, и четыре числа с двумя
    // процентами в них не влезают - строка переносилась бы и ломала таблицу.
    // Заодно пары стоят рядом: «занято» под «всего», «свободно» под ним же.
    out_puts("всего ");
    out_u64(total_kib);
    out_puts(" КБ, занято ");
    out_u64(used_kib);
    out_puts(" КБ (");
    out_percent(total - free_bytes, total);
    out_puts(")\n");

    out_puts("       свободно ");
    out_kib(free_bytes);
    out_puts(" (");
    out_percent(free_bytes, total);
    out_puts("), блок ");
    out_kib(heap_caps_get_largest_free_block(caps));
    out_puts("\n");
}

static void cmd_free(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    // Три числа на область: «всего» - бюджет, «занято»/«свободно» - запас,
    // «блок» - влезет ли буфер одним куском. Проценты рядом с кибибайтами -
    // чтобы «343 КБ свободно» не приходилось пересчитывать в голове.
    put_heap_line("SRAM", MALLOC_CAP_INTERNAL);
    put_heap_line("PSRAM", MALLOC_CAP_SPIRAM);

    const size_t used = basalt_stack_used();
    const size_t size = basalt_stack_size();
    out_puts("стек: занято ");
    out_u64(used);
    out_puts(" из ");
    out_u64(size);
    out_puts(" Б (");
    out_percent(used, size);
    out_puts("), остаток ");
    out_u64(size - used);
    out_puts(" Б\n");
}

// top. Задача у нас одна: планировщика нет, всё работает в цикле basalt_main.
// Показывать «список задач» из одной строки - не баловство: когда появится
// вторая, формат уже будет на месте, и разбирать падение станет проще.
//
// Остаток стека - не украшение. На xtensa переполнение стека не даёт внятной
// ошибки: оно молча затирает соседние данные. Поэтому число берётся из
// закраски (arch/xtensa/stack.c) и показывает самый глубокий момент.
static void cmd_top(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const size_t used = basalt_stack_used();
    const size_t size = basalt_stack_size();

    out_puts("задач 1\n");
    out_puts("  главная   выполняется   стек ");
    out_u64(used);
    out_puts("/");
    out_u64(size);
    out_puts(" Б (");
    out_percent(used, size);
    out_puts("), остаток ");
    out_u64(size - used);
    out_puts(" Б\n");

    out_puts("uptime ");
    out_u64(basalt_uptime_us() / 1000000u);
    out_puts(" с, SRAM свободно ");
    out_kib(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    out_puts(", PSRAM свободно ");
    out_kib(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    out_puts("\n");
}

// Звука на плате нет: пищалки не распаяно. Честный ответ - сказать это и
// отправить BEL в терминал мака, где он действительно пискнет.
static void cmd_beep(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    mirror_str("\a");
    out_puts("на плате нет пищалки: BEL отправлен в терминал\n");
}

static void cmd_clear(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    text_clear();
}

static void cmd_uptime(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    out_puts("аптайм ");
    out_u64(basalt_uptime_us() / 1000000u);
    out_puts(" с\n");
}

static void cmd_mirror(int argc, char** argv)
{
    if (argc < 2) {
        out_puts(s_mirror ? "зеркало включено\n" : "зеркало выключено\n");
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        // Включаем до ответа: иначе подтверждение не дошло бы до терминала.
        s_mirror = true;
        out_puts("зеркало включено\n");
        return;
    }

    if (strcmp(argv[1], "off") == 0) {
        out_puts("зеркало выключено\n");
        s_mirror = false;
        return;
    }

    out_puts("mirror on|off\n");
}

static void cmd_reboot(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    out_puts("перезагрузка\n");

    // Дать выводу дойти до экрана и до USB. Сброс сразу после печати оставил бы
    // недорисованную строку - а по ней как раз и проверяют, что команда дошла.
    present_dirty();
    basalt_delay_us(100000);

    // Через регистр, а не esp_restart: esp_restart требует поднятых сервисов
    // IDF, а мы их не поднимаем. Бит только выставляется, остальные поля
    // регистра - настройки аналоговой части, сбрасывать их в ноль нельзя.
    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    for (;;) {
    }
}

// Намеренное исключение. Нужно не для развлечения: паника - это код, который
// работает последним, и проверить её иначе, чем уронив систему, невозможно.
// Регистры, причина и сброс должны быть видны без перепрошивки платы.
static void cmd_crash(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    out_puts("намеренное исключение: запись по нулевому адресу\n");
    out_puts("дальше должна быть паника с регистрами и сброс\n");
    present_dirty();
    basalt_delay_us(200000);

    volatile uint32_t* zero = (volatile uint32_t*)0;
    *zero = 0xDEADBEEFu;

    out_puts("исключения не было - это ошибка\n");
}

static const command s_commands[] = {
    {"help",    cmd_help,    "этот список"},
    {"echo",    cmd_echo,    "напечатать аргументы"},
    {"history", cmd_history, "последние команды"},
    {"date",    cmd_date,    "время с запуска"},
    {"free",    cmd_free,    "память и стек"},
    {"top",     cmd_top,     "задачи и остаток стека"},
    {"beep",    cmd_beep,    "писк в терминал"},
    {"clear",   cmd_clear,   "очистить экран"},
    {"uptime",  cmd_uptime,  "время работы"},
    {"mirror",  cmd_mirror,  "on|off: повторять поток в USB"},
    {"reboot",  cmd_reboot,  "перезагрузка"},
    {"crash",   cmd_crash,   "проверить панику"},
};

static const size_t s_command_count = sizeof(s_commands) / sizeof(s_commands[0]);

static void cmd_help(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    out_puts("команды:\n");
    for (size_t i = 0; i < s_command_count; ++i) {
        const char* name = s_commands[i].name;
        out_puts("  ");
        out_puts(name);

        // Выравнивание по колонке, а не табуляцией: табуляцию матрица
        // понимает по-своему, а список должен читаться на экране платы.
        size_t pad = strlen(name);
        do {
            out_puts(" ");
            ++pad;
        } while (pad < 9);

        out_puts(s_commands[i].help);
        out_puts("\n");
    }

    out_puts("\n");
    out_puts("стрелки - история и курсор, Ctrl-A/E/K/U/W - правка\n");
    out_puts("Ctrl-C - бросить строку, Ctrl-D - конец ввода\n");
    out_puts("G0 на плате - тоже Ctrl-C\n");
    out_puts("кавычки и # в парсере: echo \"a b\" # комментарий\n");
}

// Разбор и запуск. Строка приходит из редактора, а он правит буфер на месте -
// поэтому работаем с копией: parse_line расставляет нули, и после него
// le_line() уже не та строка, которую набрали.
static void run_command(const char* line)
{
    char buf[LINEEDIT_CAP];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    parse_result args;
    switch (parse_line(buf, &args)) {
    case PARSE_OK:
        break;

    case PARSE_ERR_QUOTE:
        out_puts("?: кавычка не закрыта\n");
        return;

    case PARSE_ERR_ARGS:
        out_puts("?: слишком много слов\n");
        return;
    }

    if (args.argc == 0) {
        return;
    }

    for (size_t i = 0; i < s_command_count; ++i) {
        if (strcmp(args.argv[0], s_commands[i].name) == 0) {
            s_commands[i].fn(args.argc, args.argv);
            return;
        }
    }

    out_puts("?: ");
    out_puts(args.argv[0]);
    out_puts(": нет такой команды, help - список\n");
}

// --- обработка событий редактора -----------------------------------------

static void handle(le_event ev)
{
    switch (ev) {
    case LE_NONE:
        break;

    case LE_CHANGED:
        draw_line();
        break;

    case LE_SUBMIT: {
        // Строку сначала показываем в потоке целиком: на экране она уже
        // нарисована, а в терминале мака её видно только зеркалом.
        out_cp('\n');

        char line[LINEEDIT_CAP];
        strncpy(line, le_line(), sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';

        le_reset();
        run_command(line);
        prompt();
        break;
    }

    case LE_INTR:
        // ^C печатается, чтобы прерывание было видно: иначе строка просто
        // исчезнет, и это выглядело бы как сбой.
        out_puts("^C\n");
        le_reset();
        prompt();
        break;

    case LE_EOF:
        s_halt = true;
        out_puts("^D конец ввода, RST - заново\n");
        break;

    case LE_BELL:
        // Стирать нечего или строка полна. На экране обратной связи нет
        // намеренно: любая заметка сдвинула бы строку, которую правят. Звук
        // уходит в терминал мака, где он и слышен.
        mirror_str("\a");
        break;

    case LE_REDRAW:
        // Ctrl-L: экран заново. Приглашение и строку печатаем с чистого листа,
        // иначе после очистки на экране не осталось бы даже того, что набрано.
        text_clear();
        prompt();
        draw_line();
        break;
    }
}

// Один байт из USB - через tty, событие - в редактор. Отдельной функцией,
// потому что тем же путём идёт кнопка G0: прерывание должно проходить ровно
// тот же разбор, что и байт из терминала, иначе поведение кнопки и клавиши
// однажды разойдётся.
static void feed_byte(uint8_t byte)
{
    const tty_result r = tty_input(byte);

    switch (r.event) {
    case TTY_EV_CHAR:
        handle(le_input(r.cp));
        break;

    case TTY_EV_INTR:
        handle(le_input(0x03));
        break;

    default:
        // В raw-режиме остальных событий не бывает: строк там нет по
        // определению.
        break;
    }
}

// Кнопка G0 как Ctrl-C: терминал на маке часто перехватывает ^C сам, и байт
// 0x03 до платы не доходит. Фронт, а не уровень: удержание не должно слать
// прерывание каждый проход цикла.
static bool s_g0_down;
static uint64_t s_g0_change_us = 20000u;

static void poll_button(void)
{
    const bool down = button_down(BUTTON_PRIMARY);
    if (down == s_g0_down) {
        return;
    }

    const uint64_t now = basalt_uptime_us();
    if (now - s_g0_change_us < 20000u) {
        return;
    }
    s_g0_change_us = now;
    s_g0_down = down;

    // Через tty, а не напрямую в редактор: прерывание должно опознаваться там
    // же, где опознаётся байт из USB.
    if (down && !s_halt) {
        feed_byte(0x03);
    }
}

// --- старт ----------------------------------------------------------------

void basalt_main(void)
{
    // Порядок важен трижды, и по разным причинам.
    //
    // 1. Консоль первой: если что-то ниже зависнет, след останется в логе.
    // 2. Экран - до паузы. Панель ST7789 хранит кадр в своей памяти, пока на
    //    ней есть питание, поэтому после сброса на экране висит картинка
    //    прошлой сессии, пока мы её не затрём. Пауза в 2 с нужна хосту, чтобы
    //    открыть порт, и без стирания получается, что плата уже перезагрузилась,
    //    а на экране ещё старый текст - легко принять за «состояние
    //    восстановилось». Поэтому сначала стираем, потом ждём.
    // 3. Векторы - после паузы: их отчёт печатается в консоль, и без паузы он
    //    ушёл бы в никуда, потому что порт на маке ещё не открыт.
    basalt_console_init();
    basalt_tick_init();

    lcd_init();
    lcd_backlight(true);
    text_init();
    present_dirty();

    basalt_delay_us(2000000);
    basalt_vectors_init();

    basalt_put("\n\nbasalt-os mini, шелл\n");

    tty_init();
    tty_set_mode(TTY_MODE_RAW);
    button_init();
    le_init();

    out_puts("basalt-os mini\n");
    out_puts("\n");
    out_puts("help - список команд\n");
    out_puts("\n");

    basalt_put("[shell] экран ");
    basalt_put_u64(TEXT_COLS);
    basalt_put("x");
    basalt_put_u64(TEXT_ROWS);
    basalt_put(", tty raw, стек ");
    basalt_put_u64(basalt_stack_size());
    basalt_put(" Б\n");
    basalt_put("ready\n\n");

    prompt();

    uint64_t next_blink = basalt_uptime_us() + 500000u;
    bool blink = true;

    for (;;) {
        // Приняли всё, что есть, и только потом рисуем: вставка строки из
        // буфера обмена - это одна передача, а не перерисовка на каждый символ.
        bool activity = false;
        if (!s_halt) {
            uint8_t buf[64];
            const size_t got = basalt_console_read(buf, sizeof(buf));
            for (size_t i = 0; i < got; ++i) {
                feed_byte(buf[i]);
            }
            activity = (got > 0);
        }

        poll_button();

        const uint64_t now = basalt_uptime_us();
        if (activity) {
            // Нажатие показывает курсор сразу и сдвигает фазу мигания: иначе
            // подчёркивание появится только через полсекунды, и кажется, что
            // ввод не дошёл до платы.
            blink = true;
            text_cursor_blink(true);
            next_blink = now + 500000u;
        } else if (now >= next_blink) {
            next_blink += 500000u;
            blink = !blink;
            text_cursor_blink(blink);
        }

        present_dirty();

        basalt_delay_us(500);
    }
}
