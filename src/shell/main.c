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

#include "bfs.h"
#include "bfs_flash.h"
#include "button.h"
#include "calendar.h"
#include "clock.h"
#include "console_usb_jtag.h"
#include "duration.h"
#include "editor.h"
#include "esp_heap_caps.h"
#include "format.h"
#include "hexdump.h"
#include "history.h"
#include "lcd.h"
#include "lineedit.h"
#include "matrix.h"
#include "pager.h"
#include "parse.h"
#include "redirect.h"
#include "stack.h"
#include "start.h"
#include "tick.h"
#include "int1.h"
#include "idle.h"
#include "tasks.h"
#include "tty.h"
#include "utf8.h"
#include "vectors.h"
#include "vfs.h"
#include "vfs_bfs.h"
#include "xmodem.h"

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

// Часы нужны и командам выше по файлу (reboot сохраняет время перед сбросом),
// поэтому объявления стоят здесь, а определения - ниже, в блоке времени.
static bool shell_now(uint32_t* out);
static int fs_save_clock(uint32_t unix_secs);

// Счётчик итераций демо-задачи. Объявление здесь по той же причине: cmd_top
// стоит выше определения задачи.
uint32_t shell_task_iterations(void);

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

// --- куда идёт вывод команды ----------------------------------------------
//
// Три приёмника, а не один дескриптор: экран, файл и конвейер. Экран - это
// текстовая матрица плюс зеркало в USB; файл и конвейер принимают байты.
// Переключает приёмник выполнение строки, поэтому команды про перенаправления
// ничего не знают - и знать не должны.

typedef enum {
    SINK_SCREEN = 0,
    SINK_FILE,
    SINK_PIPE,
} out_sink;

static out_sink s_sink = SINK_SCREEN;
static int s_out_fd = -1;
static bool s_out_failed;

// Команда отказалась писать в этот файл (например, он же и источник). Тогда
// приёмник не трогает файл вообще: без этого `cat /a > /a` успел бы обнулить
// его ещё до чтения.
static bool s_out_cancel;

// Усечение отложено до первой записи. Причина та же: `cat /a > /a` открывается
// на запись раньше, чем команда увидит, что читает тот же файл, и `O_TRUNC` в
// этот момент уже уничтожил бы данные. Пустой вывод по-прежнему даёт пустой
// файл - усечение делается в конце ступени, если писать так и не начали.
static const char* s_out_path;
static bool s_out_trunc_pending;

// Канонический путь файла вывода - чтобы сравнивать с ним пути, которые
// команде дали в argv (`cat /a > ./a` - тот же файл).
static char s_out_canon[VFS_PATH_MAX];

// Запись в файл копится в буфере размером с блок ФС. BFS переписывает блок
// целиком, поэтому каждый мелкий vfs_write - это отдельная запись во flash
// (~десятки миллисекунд). `help > файл` без буфера делает больше сотни таких
// записей и выглядит как зависание: минуты без признаков жизни.
#define OUT_BUF_SIZE 4096u
static uint8_t s_out_buf[OUT_BUF_SIZE];
static size_t s_out_len;

// Конвейер без процессов: вывод предыдущей ступени копится целиком, следующая
// читает его. Отсюда предел и честная ошибка, когда не влезло: молча
// обрезанный вывод хуже отказа, потому что выглядит как правильный.
//
// Место - PSRAM: это ровно тот случай, для которого она нужна (буфер под
// данные, а не под горячий код). Два буфера, потому что ступень читает из
// одного и пишет в другой; если PSRAM на плате нет, годится и обычная память.
#define PIPE_BUF_SIZE (64u * 1024u)
static uint8_t* s_pipe[2];
static int s_pipe_slot;
static uint32_t s_pipe_len;
static bool s_pipe_overflow;

// Откуда команда читает, если ей не дали имени файла: `< файл` или конвейер.
static int s_in_fd = -1;
static bool s_stdin_pipe;
static const uint8_t* s_pipe_in;
static uint32_t s_pipe_in_len;
static uint32_t s_pipe_in_pos;

static uint8_t* pipe_buffer(int slot)
{
    if (s_pipe[slot] != NULL) {
        return s_pipe[slot];
    }
    s_pipe[slot] = heap_caps_malloc(PIPE_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pipe[slot] == NULL) {
        s_pipe[slot] = heap_caps_malloc(PIPE_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return s_pipe[slot];
}

// Куда пишем на самом деле, в обход буфера. Только для приёмника файла.
static void out_file_write(const uint8_t* data, size_t len)
{
    if (s_out_fd < 0 || s_out_failed) {
        return;
    }

    if (s_out_trunc_pending) {
        s_out_trunc_pending = false;
        if (vfs_truncate(s_out_path, 0) != VFS_OK) {
            s_out_failed = true;
            return;
        }
    }

    size_t done = 0;
    while (done < len) {
        uint32_t put = 0;
        const int rc = vfs_write(s_out_fd, data + done, (uint32_t)(len - done), &put);
        if (rc != VFS_OK || put == 0) {
            s_out_failed = true;
            return;
        }
        done += put;
    }
}

static void out_file_flush(void)
{
    if (s_out_len == 0) {
        return;
    }
    const size_t n = s_out_len;
    s_out_len = 0;
    out_file_write(s_out_buf, n);
}

static void sink_bytes(const uint8_t* data, size_t len)
{
    if (s_sink == SINK_PIPE) {
        uint8_t* const buf = pipe_buffer(s_pipe_slot);
        if (buf == NULL || len > PIPE_BUF_SIZE - s_pipe_len) {
            s_pipe_overflow = true;
            return;
        }
        memcpy(buf + s_pipe_len, data, len);
        s_pipe_len += (uint32_t)len;
        return;
    }

    if (s_sink == SINK_FILE) {
        if (s_out_fd < 0 || s_out_failed || s_out_cancel) {
            return;
        }
        while (len > 0) {
            const size_t room = OUT_BUF_SIZE - s_out_len;
            const size_t n = (len < room) ? len : room;
            memcpy(s_out_buf + s_out_len, data, n);
            s_out_len += n;
            data += n;
            len -= n;
            if (s_out_len == OUT_BUF_SIZE) {
                out_file_flush();
            }
        }
    }
}

// Тот же ли это файл, что сейчас открыт на запись. Команда, которая собирается
// читать файл, обязана спросить: иначе вывод затрёт вход, и прочитать будет
// уже нечего. Возвращает false, если путь не разобрать - тогда решает команда.
static bool sink_is_input(const char* path)
{
    if (s_out_canon[0] == '\0') {
        return false;
    }
    char canon[VFS_PATH_MAX];
    if (vfs_resolve(path, canon, sizeof(canon)) != VFS_OK) {
        return false;
    }
    return strcmp(canon, s_out_canon) == 0;
}

// Команда нашла, что вход и выход - один файл. Печатает она сама (текст
// зависит от команды), а здесь только запрет на запись и усечение.
static void sink_cancel(void)
{
    s_out_cancel = true;
}

// Сообщения самой оболочки и ошибки команд. В отличие от out_puts идут на
// экран всегда: `cat /nope > f` должен ругаться на экране, а не писать
// «не найдено» в новый файл. В настоящем шелле для этого есть отдельный поток
// ошибок - у нас его нет, значит нужна отдельная функция, а не флаг.
static void out_diag(const char* s)
{
    if (s == NULL) {
        return;
    }
    text_puts(s);
    mirror_stream(s);
}

static void out_puts(const char* s)
{
    if (s == NULL) {
        return;
    }
    if (s_sink != SINK_SCREEN) {
        sink_bytes((const uint8_t*)s, strlen(s));
        return;
    }
    text_puts(s);
    mirror_stream(s);
}

static void out_cp(uint32_t cp)
{
    if (s_sink != SINK_SCREEN) {
        char buf[4];
        const size_t n = utf8_encode(cp, buf);
        sink_bytes((const uint8_t*)buf, n);
        return;
    }
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

// Hex с фиксированным числом знаков. Нужен именно отдельной функцией, а не
// «out_u64 после 0x»: однажды PS уже напечатали десятичным после «0x», и
// 0x395042 в логе прочитали как PS с WOE=0, хотя настоящий PS был 0x60722.
// Ошибка чтения стоила правки в ассемблере, которой там быть не должно было.
static void out_hex(uint32_t v, int digits)
{
    char buf[9];
    static const char kDigits[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; --i) {
        buf[i] = kDigits[v & 0xFu];
        v >>= 4;
    }
    buf[digits] = '\0';
    out_puts(buf);
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
static const char* task_state_text(basalt_task_state_t st)
{
    switch (st) {
    case BASALT_TASK_RUNNING:
        return "выполняется";
    case BASALT_TASK_READY:
        return "готова";
    case BASALT_TASK_DONE:
        return "завершена";
    default:
        return "свободна";
    }
}

static void cmd_top(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const size_t count = basalt_task_count();
    out_puts("задач ");
    out_u64(count);
    out_puts("\n");

    for (size_t i = 0; i < count; ++i) {
        basalt_task_info_t info;
        if (!basalt_task_get(i, &info)) {
            continue;
        }

        // Отметка текущей задачи: по ней видно, кто именно держит процессор в
        // момент вызова top, а не «какая-то из них».
        out_puts(i == basalt_task_current() ? "  * " : "    ");
        out_puts(info.name);

        // Выравнивание пробелами, а не табуляцией: матрица понимает табуляцию
        // по-своему, а колонки должны сходиться на экране платы.
        size_t pad = strlen(info.name) + 2;
        do {
            out_puts(" ");
            ++pad;
        } while (pad < 11);

        out_puts(task_state_text(info.state));

        // У главной задачи стека в планировщике нет: его задал start.S, и
        // размер знает только stack.c. Показываем тем же способом, что и
        // раньше, - иначе строка главной задачи стала бы пустой.
        if (info.stack_size == 0) {
            const size_t used = basalt_stack_used();
            const size_t size = basalt_stack_size();
            out_puts("   стек ");
            out_u64(used);
            out_puts("/");
            out_u64(size);
            out_puts(" Б (");
            out_percent(used, size);
            out_puts(")");
        } else {
            out_puts("   стек ");
            out_u64(info.stack_used);
            out_puts("/");
            out_u64(info.stack_size);
            out_puts(" Б (");
            out_percent(info.stack_used, info.stack_size);
            out_puts(")");
        }

        out_puts(", смен ");
        out_u64(info.switches);
        out_puts("\n");
    }

    out_puts("переключений ");
    out_u64(basalt_task_switches());
    out_puts("\n");

    out_puts("uptime ");
    out_u64(basalt_uptime_us() / 1000000u);
    out_puts(" с, тиков ");
    out_u64(basalt_int1_ticks());
    out_puts(" (");
    out_u64(BASALT_INT1_HZ);
    out_puts(" Гц)\n");

    // Сон главного цикла: раз в тик при простое. Число заметно больше тиков
    // означало бы, что работу будят чужие прерывания; ноль - что сон не
    // включён и цикл по-прежнему крутится.
    out_puts("сон ");
    out_u64(basalt_idle_waits());
    out_puts(" раз\n");

    // Итерации демо-задачи: если это число не растёт между двумя вызовами
    // top, планировщик не переключает, и всё остальное в отчёте - украшение.
    out_puts("пульс ");
    out_u64(shell_task_iterations());
    out_puts(" итераций\n");

    out_puts("SRAM свободно ");
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

// Подождать. Ждём опросом: планировщика в системе нет, усыпить ядро нечем.
// Поэтому `sleep 5` - это пять секунд, в течение которых прерывание Ctrl-C не
// придёт: консоль читается только в главном цикле, а он стоит здесь. Так честнее
// и понятнее, чем изображать прерываемый сон через кнопку, которую никто не
// нажмёт.
static void cmd_sleep(int argc, char** argv)
{
    if (argc < 2) {
        out_diag("sleep <длительность>: 5, 300ms, 1m30s\n");
        return;
    }

    uint32_t ms = 0;
    if (!duration_parse(argv[1], &ms)) {
        out_diag("sleep: негодная длительность (до 24 ч): ");
        out_diag(argv[1]);
        out_diag("\n");
        return;
    }

    // Зеркало получает сообщение до сна: иначе в терминале мака между командой
    // и результатом висела бы пустота, и казалось бы, что плата зависла.
    present_dirty();
    char text[32];
    out_puts("сплю ");
    out_puts(duration_text(text, sizeof(text), ms));
    out_puts("\n");
    present_dirty();

    // Спит ядро, а не счётчик: до этого шага здесь был цикл опроса, и десять
    // секунд `sleep` означали десять секунд полной загрузки процессора.
    basalt_idle_ms(ms);

    out_puts("проснулся\n");
}

// Будильник: мигание подсветкой через заданный интервал. Звука на плате нет -
// пищалки не распаяно, поэтому сигнал визуальный. Подсветка, а не текст на
// экране: мигание видно с другого конца стола, и оно не затирает то, что уже
// написано.
static void cmd_alarm(int argc, char** argv)
{
    if (argc < 2) {
        out_diag("alarm <через сколько>: 10s, 1m\n");
        return;
    }

    uint32_t ms = 0;
    if (!duration_parse(argv[1], &ms)) {
        out_diag("alarm: негодная длительность (до 24 ч): ");
        out_diag(argv[1]);
        out_diag("\n");
        return;
    }

    present_dirty();
    char text[32];
    out_puts("будильник через ");
    out_puts(duration_text(text, sizeof(text), ms));
    out_puts(", Ctrl-C для отмены\n");

    const uint64_t until = basalt_uptime_us() + (uint64_t)ms * 1000u;

    // Ожидание с проверкой кнопки и консоли: будильник, который нельзя
    // отменить, ставит систему в известное состояние на минуту вперёд, и это
    // раздражает сильнее, чем помогает.
    while (basalt_uptime_us() < until) {
        if (button_down(BUTTON_PRIMARY)) {
            basalt_delay_us(30000);
            if (button_down(BUTTON_PRIMARY)) {
                out_puts("будильник отменён\n");
                return;
            }
        }

        uint8_t b = 0;
        if (basalt_console_read(&b, 1) > 0 && b == 0x03) {
            out_puts("^C будильник отменён\n");
            return;
        }
        basalt_delay_us(2000);
    }

    // Пять секунд мигания: короткое мигание можно пропустить, если в этот
    // момент смотришь не на плату.
    for (int i = 0; i < 10; ++i) {
        lcd_backlight((i % 2) == 0);
        basalt_delay_us(500000);
    }
    lcd_backlight(true);

    mirror_str("\a");
    out_puts("будильник!\n");
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

// Проверка частоты тика секундомером, а не на глаз.
//
// Зачем отдельная команда: «тики растут» само по себе ничего не доказывает -
// они будут расти и при периоде втрое больше задуманного. Здесь частота
// измеряется как (прирост тиков) / (прошедшее время по SYSTIMER), и оба
// источника независимы: тики идут от CCOUNT (тактовая частота ядра), время -
// от SYSTIMER (16 МГц). Совпадение с BASALT_INT1_HZ и есть проверка того, что
// период посчитан по настоящей частоте ядра.
static void cmd_tickrate(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const uint32_t ms = 1000;

    out_puts("меряю тик ");
    out_u64(ms);
    out_puts(" мс...\n");
    present_dirty();

    const uint32_t t0 = basalt_int1_ticks();
    const uint64_t us0 = basalt_uptime_us();
    while (basalt_uptime_us() - us0 < (uint64_t)ms * 1000u) {
    }
    const uint32_t t1 = basalt_int1_ticks();
    const uint64_t us1 = basalt_uptime_us();

    const uint32_t ticks = t1 - t0;
    const uint64_t us = us1 - us0;
    if (us == 0) {
        out_diag("tickrate: время не идёт\n");
        return;
    }

    // Целая часть и десятая доля, без плавающей точки.
    const uint64_t hz10 = (uint64_t)ticks * 10000000u / us;
    out_puts("тиков ");
    out_u64(ticks);
    out_puts(" за ");
    out_u64(us / 1000u);
    out_puts(" мс: ");
    out_u64(hz10 / 10u);
    out_puts(".");
    out_u64(hz10 % 10u);
    out_puts(" Гц, ожидалось ");
    out_u64(BASALT_INT1_HZ);
    out_puts(", период ");
    out_u64(basalt_int1_period());
    out_puts(" тактов при ядре ");
    out_u64(basalt_int1_cpu_hz() / 1000000u);
    out_puts(" МГц\n");

    basalt_int1_state_view_t v;
    basalt_int1_view(&v);
    out_puts("  INTENABLE 0x");
    out_hex(v.intenable, 8);
    out_puts(" INTERRUPT 0x");
    out_hex(v.interrupt, 8);
    out_puts(" PS 0x");
    out_hex(v.ps, 8);
    out_puts("\n  CCOUNT ");
    out_u64(v.ccount);
    out_puts(" CCOMPARE0 ");
    out_u64(v.ccompare);
    out_puts(v.ccount >= v.ccompare ? " (просит прерывания)\n" : " (ждёт)\n");
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

    // Время перед сбросом сохраняется: после загрузки часы поднимутся из
    // /etc/clock, и «время выключения» станет моментом перезагрузки, а не
    // моментом, когда время установили последний раз. Без этого `date`
    // после перезагрузки отставал бы на всю сессию.
    uint32_t now = 0;
    if (shell_now(&now)) {
        (void)fs_save_clock(now);
    }

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

// --- файлы ----------------------------------------------------------------
//
// Команды над файлами - единственное место в оболочке, которое знает про VFS.
// Пути, текущий каталог и таблица открытых файлов живут в storage/vfs.c;
// здесь остаются argv, вывод и решения вроде «rm каталога - это rmdir».

// Раздел storage и ФС на нём. В статике: монтирование одно на систему, а VFS
// держит указатель на bfs_t всё время работы.
static bfs_t s_fs;

// Корень смонтирован. Отдельный флаг, а не проверка указателей: при отказе
// хранилища система поднимается без файлов, и всё файловое должно это видеть,
// а не выяснять по коду ошибки от каждой операции.
static bool s_fs_ready;

// Текст ошибки. Свои коды монтирования (BASALT_FS_*) и коды VFS_* численно
// пересекаются, поэтому проверяются первыми.
static void out_fs_error(int rc)
{
    switch (rc) {
    case BASALT_FS_ERR_NO_FLASH:
        out_diag("драйвер flash не завёлся");
        return;
    case BASALT_FS_ERR_NO_PARTITION:
        out_diag("в таблице разделов нет раздела storage");
        return;
    case BASALT_FS_ERR_BAD_PARTITION:
        out_diag("раздел storage негоден: не выровнен или не того размера");
        return;
    case BFS_ERR_BROKEN:
        out_diag("ФС сломана, нужен перезапуск");
        return;
    case BFS_ERR_FORMAT:
        out_diag("раздел не размечен");
        return;
    default:
        out_diag(vfs_strerror(rc));
        return;
    }
}

// Смонтировать корень. Вызывается один раз при старте: дальше таблица точек
// монтирования не меняется - размонтирования в интерфейсе нет.
//
// format_if_empty=true здесь - осознанное решение: раздел storage наш по
// таблице разделов, и «первый запуск» - единственный случай, когда он пуст.
// Отказ монтировать оставил бы систему без файлов до ручной команды, которой
// пока нет.
static void fs_mount_root(void)
{
    bool formatted = false;
    int rc = basalt_fs_mount(&s_fs, "storage", true, &formatted);
    if (rc != 0) {
        out_diag("файлы недоступны: ");
        out_fs_error(rc);
        out_diag("\n");
        basalt_put("[shell] ФС не смонтирована\n");
        return;
    }

    vfs_init();
    const vfs_backend_t be = vfs_bfs_backend(&s_fs);
    rc = vfs_mount("/", &be);
    if (rc != VFS_OK) {
        out_diag("файлы недоступны: ");
        out_diag(vfs_strerror(rc));
        out_diag("\n");
        basalt_put("[shell] VFS не смонтирован\n");
        return;
    }

    s_fs_ready = true;

    basalt_put("[shell] ФС storage: ");
    basalt_put(formatted ? "размечена заново\n" : "смонтирована\n");
}

// --- /etc/rc: что делать при старте ---------------------------------------
//
// Зачем системе создавать этот файл самой. После `pio run -t erase` корень
// пуст, и записать «что делать при старте» негде: первый запуск ничем не
// отличается от второго, а любая настройка живёт до перезагрузки. Пустой
// шаблон с объяснением внутри решает это без отдельной команды `mkfs -c`.
//
// Файл создаётся один раз и больше не перезаписывается: правки пользователя
// важнее шаблона, а затереть их при обновлении прошивки было бы худшим из
// возможных сюрпризов.

#define RC_DIR  "/etc"
#define RC_PATH "/etc/rc"

static const char s_rc_default[] =
    "# /etc/rc - команды, которые выполняются при старте системы.\n"
    "# По одной в строке, как в шелле; # - комментарий.\n"
    "# Например: echo привет\n";

static void run_command(const char* line);

static void fs_ensure_rc(void)
{
    if (!s_fs_ready) {
        return;
    }

    // Каталог мог остаться от прошлой жизни, а мог и не быть: и то и другое
    // нормально, поэтому «уже есть» - не ошибка.
    const int mk = vfs_mkdir(RC_DIR);
    if (mk != VFS_OK && mk != VFS_ERR_EXISTS) {
        out_diag(RC_DIR ": ");
        out_fs_error(mk);
        out_diag("\n");
        return;
    }

    vfs_stat_t st;
    if (vfs_stat(RC_PATH, &st) == VFS_OK) {
        return;
    }

    int fd = -1;
    int rc = vfs_open(RC_PATH, VFS_O_WRONLY | VFS_O_CREATE, &fd);
    if (rc != VFS_OK) {
        out_diag(RC_PATH ": ");
        out_fs_error(rc);
        out_diag("\n");
        return;
    }

    const uint32_t len = (uint32_t)(sizeof(s_rc_default) - 1);
    uint32_t put = 0;
    rc = vfs_write(fd, s_rc_default, len, &put);
    vfs_close(fd);

    if (rc != VFS_OK || put != len) {
        out_diag(RC_PATH ": не удалось записать шаблон\n");
    }
}

// Выполнить /etc/rc строка за строкой. Свой разборщик строк, а не общий с
// редактором: файл приходит с мака, где конец строки - CRLF, и `\r` в конце
// команды сломал бы последнее слово (`rm /x` + CR = имя `/x\r`).
//
// Строку длиннее буфера редактора пропускаем целиком, а не режем: обрезанная
// команда выполнилась бы не с теми аргументами, и это хуже, чем не выполнена
// вовсе.
#define RC_LINE_MAX LINEEDIT_CAP

static void fs_run_rc(void)
{
    if (!s_fs_ready) {
        return;
    }

    int fd = -1;
    if (vfs_open(RC_PATH, VFS_O_RDONLY, &fd) != VFS_OK) {
        return;  // файла нет - нечего выполнять, и это не ошибка
    }

    char line[RC_LINE_MAX];
    size_t len = 0;
    bool overlong = false;
    uint32_t commands = 0;
    uint8_t chunk[128];

    for (;;) {
        uint32_t got = 0;
        if (vfs_read(fd, chunk, sizeof(chunk), &got) != VFS_OK || got == 0) {
            break;
        }

        for (uint32_t i = 0; i < got; ++i) {
            const char c = (char)chunk[i];

            if (c != '\n') {
                if (c == '\r') {
                    continue;
                }
                if (len + 1 < sizeof(line)) {
                    line[len++] = c;
                } else {
                    overlong = true;
                }
                continue;
            }

            line[len] = '\0';
            len = 0;

            if (overlong) {
                out_diag(RC_PATH ": строка слишком длинная, пропущена\n");
                overlong = false;
                continue;
            }
            if (line[0] == '\0') {
                continue;
            }

            run_command(line);
            ++commands;
        }
    }

    // Последняя строка может быть без перевода строки: файл, собранный на
    // маке или записанный `echo` без него, - обычное дело.
    line[len] = '\0';
    if (!overlong && len > 0) {
        run_command(line);
        ++commands;
    }

    vfs_close(fd);

    // «Прочитано», а не «выполнено»: комментарии и пустые строки тоже
    // считаются, и называть их выполненными командами было бы неправдой.
    basalt_put("[shell] /etc/rc: строк: ");
    basalt_put_u64(commands);
    basalt_put("\n");
}

// Размер в байтах. Единица печатается всегда: «12» без неё читалось бы и как
// блоки, и как кибибайты.
static void out_bytes(uint32_t size)
{
    out_u64(size);
    out_puts(" Б");
}

static void out_path_error(int rc)
{
    out_fs_error(rc);
    out_diag("\n");
}

static size_t str_app(char* out, size_t cap, size_t at, const char* s);

// Дата и время. Часов реального времени у платы нет: нет ни RTC-микросхемы с
// батарейкой, ни NTP. Но время всё равно нужно - хотя бы для меток файлов,
// иначе `ls -l` бессмыслен. Поэтому время в системе устроено так:
//
//   - при старте оно не установлено, и `date` говорит об этом прямо, а не
//     показывает 1970-й;
//   - установить его можно командой `date set`, и оно сохраняется в
//     /etc/clock;
//   - при следующем старте время берётся из /etc/clock и идёт от момента,
//     когда его записали. Плата была выключена - значит отстаёт ровно на это
//     время, и это единственное, что можно сделать без RTC.
//
// Отдельного файла настроек нет: /etc/clock - обычный файл в корне ФС, и
// его можно посмотреть, перезаписать и удалить теми же командами, что и всё
// остальное.
#define CLOCK_PATH "/etc/clock"

static clock_t s_clock;

// Прочитать сохранённое время и поставить часы. Файла нет - не ошибка:
// первый запуск после разметки ничем не отличается от второго, просто время
// останется неустановленным.
static void fs_load_clock(void)
{
    if (!s_fs_ready) {
        return;
    }

    clock_reset(&s_clock);

    int fd = -1;
    if (vfs_open(CLOCK_PATH, VFS_O_RDONLY, &fd) != VFS_OK) {
        return;
    }

    char text[64];
    uint32_t got = 0;
    const int rc = vfs_read(fd, text, sizeof(text) - 1, &got);
    vfs_close(fd);
    if (rc != VFS_OK || got == 0) {
        return;
    }
    text[got] = '\0';

    uint32_t unix_secs = 0;
    if (!cal_parse(text, &unix_secs)) {
        // Файл испорчен - это не повод не работать. Метка неизвестна, и
        // `date` так и скажет; молча подставлять «сейчас» значило бы соврать.
        basalt_put("[shell] /etc/clock не разобран, время не установлено\n");
        return;
    }

    clock_set(&s_clock, unix_secs, basalt_uptime_us());
    vfs_set_now(unix_secs);

    basalt_put("[shell] время из /etc/clock, часы идут с запуска\n");
}

// Записать текущее время в /etc/clock. Вызывается после `date set` и перед
// перезагрузкой: в первом случае - чтобы время пережило выключение, во
// втором - чтобы «время последнего включения» было честным, а не моментом
// первой установки.
static int fs_save_clock(uint32_t unix_secs)
{
    if (!s_fs_ready) {
        return VFS_ERR_NO_MOUNT;
    }

    char text[CAL_TEXT_MAX];
    cal_text(text, sizeof(text), unix_secs);
    const size_t len = strlen(text);

    int fd = -1;
    int rc = vfs_open(CLOCK_PATH, VFS_O_WRONLY | VFS_O_CREATE | VFS_O_TRUNC, &fd);
    if (rc != VFS_OK) {
        return rc;
    }
    uint32_t put = 0;
    rc = vfs_write(fd, text, (uint32_t)len, &put);
    vfs_close(fd);
    if (rc == VFS_OK && put != len) {
        return VFS_ERR_IO;
    }
    return rc;
}

// Где взять «сейчас». Часы не читаются модулем напрямую, поэтому показание
// счётчика передаётся сюда.
static bool shell_now(uint32_t* out)
{
    return clock_now(&s_clock, basalt_uptime_us(), out);
}

static void cmd_date(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "set") == 0) {
        // Две формы: `date set 2026-11-07 18:42:03` (дата и время
        // отдельными словами, так удобнее набирать) и одной строкой.
        char text[CAL_TEXT_MAX + 8];
        if (argc == 4) {
            // Склеиваем вручную: snprintf тянет newlib ради одной операции, а
            // str_app уже есть и обрезает по размеру. Возврат каждого вызова
            // надо использовать: str_app сам ставит ноль за последним
            // символом, поэтому прибавка к позиции оставила бы этот ноль
            // посреди строки и разбор увидел бы одну дату без времени.
            size_t at = str_app(text, sizeof(text), 0, argv[2]);
            at = str_app(text, sizeof(text), at, " ");
            str_app(text, sizeof(text), at, argv[3]);
        } else if (argc == 3) {
            str_app(text, sizeof(text), 0, argv[2]);
        } else {
            out_diag("date set ГГГГ-ММ-ДД ЧЧ:ММ[:СС]\n");
            return;
        }

        uint32_t unix_secs = 0;
        if (!cal_parse(text, &unix_secs)) {
            out_diag("date set: негодная дата, нужно ГГГГ-ММ-ДД ЧЧ:ММ[:СС]\n");
            return;
        }

        clock_set(&s_clock, unix_secs, basalt_uptime_us());
        vfs_set_now(unix_secs);

        // Часы идут и без файла, но тогда время потеряется при сбросе.
        // Сообщаем об этом, а не делаем вид, что всё сохранено.
        const int rc = fs_save_clock(unix_secs);
        if (rc != VFS_OK) {
            out_diag("date set: время установлено, но ");
            out_diag(CLOCK_PATH);
            out_diag(" не записан: ");
            out_diag(vfs_strerror(rc));
            out_diag("\n");
            return;
        }

        char buf[CAL_TEXT_MAX];
        out_puts("время: ");
        out_puts(cal_text(buf, sizeof(buf), unix_secs));
        out_puts(", сохранено в ");
        out_puts(CLOCK_PATH);
        out_puts("\n");
        return;
    }

    uint32_t now = 0;
    const bool synced = shell_now(&now);

    const uint64_t total = basalt_uptime_us() / 1000000u;
    const uint64_t days = total / 86400u;
    const uint64_t rest = total % 86400u;

    if (synced) {
        char buf[CAL_TEXT_MAX];
        out_puts(cal_text(buf, sizeof(buf), now));
        out_puts(" ");
        out_puts(cal_weekday_short(cal_weekday(now)));

        // Сказать, откуда время, - не украшение: если часы подняты из
        // /etc/clock и идут с запуска, дата отстаёт на время выключения, и
        // знать это важно.
        const uint32_t age = clock_age_secs(&s_clock, basalt_uptime_us());
        out_puts(" (с запуска ");
        out_u64(age);
        out_puts(" с)\n");
    } else {
        out_puts("календарь не установлен: date set ГГГГ-ММ-ДД ЧЧ:ММ\n");
    }

    out_puts("uptime: ");
    out_u64(days);
    out_puts(" д ");
    out_u64_2(rest / 3600u);
    out_puts(":");
    out_u64_2((rest % 3600u) / 60u);
    out_puts(":");
    out_u64_2(rest % 60u);
    out_puts("\n");
}


static void cmd_ls(int argc, char** argv)
{
    // -l - подробный вид: дата, размер, имя. Флаг отдельным словом, как в
    // обычном ls; сочетания вроде -la не поддерживаются, пока других флагов
    // нет: один флаг не стоит разбора строки.
    bool long_view = false;
    const char* path = ".";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-l") == 0) {
            long_view = true;
        } else {
            path = argv[i];
        }
    }

    char name[VFS_NAME_MAX + 1];
    uint32_t count = 0;

    for (uint32_t i = 0;; ++i) {
        vfs_stat_t st;
        const int rc = vfs_readdir(path, i, name, &st);
        if (rc == VFS_ERR_NOT_FOUND) {
            break;
        }
        if (rc != VFS_OK) {
            out_path_error(rc);
            return;
        }

        if (long_view) {
            // Дата фиксированной ширины: колонка должна читаться сверху
            // вниз. Нет времени - прочерк, а не 1970-й: «время неизвестно»
            // и «создан в прошлом веке» - разные вещи.
            if (st.mtime != 0) {
                char when[CAL_TEXT_MAX];
                out_puts(cal_text_short(when, sizeof(when), st.mtime));
            } else {
                // Столько же знаков, сколько у настоящей даты («07.11 18:42»):
                // иначе колонка разъезжается там, где время неизвестно.
                out_puts("--.-- --:--");
            }
            out_puts("  ");

            // Размер в колонку из шести знаков: без выравнивания числа
            // разной длины сдвигают имена, и список перестаёт читаться.
            char num[21];
            const char* const s = basalt_format_u64(num, sizeof(num), st.size);
            for (size_t pad = strlen(s); pad < 6; ++pad) {
                out_puts(" ");
            }
            out_puts(s);
            out_puts("  ");
        }

        out_puts(name);
        if (st.type == VFS_TYPE_DIR) {
            out_puts("/\n");
        } else {
            if (!long_view) {
                out_puts("  ");
                out_bytes(st.size);
            }
            out_puts("\n");
        }
        ++count;
    }

    if (count == 0) {
        out_puts("(пусто)\n");
    }
}

// Буфер чтения в SRAM и маленький намеренно: cat файла на сотню килобайт не
// должен упираться в «нет памяти». 512 Б - это меньше сектора, поэтому чтение
// с диска всё равно идёт кусками по блокам внутри ФС.
static uint8_t s_cat_buf[512];

static bool stdin_ready(void)
{
    return s_stdin_pipe || s_in_fd >= 0;
}

// Чтение из stdin: либо из буфера предыдущей ступени конвейера, либо из файла
// после `<`. Ноль прочитанных байт на любой из них значит «конец», а не
// ошибку, - так же, как у обычного read.
static int stdin_read(uint8_t* dst, uint32_t cap, uint32_t* got)
{
    *got = 0;
    if (s_stdin_pipe) {
        const uint32_t left = s_pipe_in_len - s_pipe_in_pos;
        const uint32_t n = (left < cap) ? left : cap;
        if (n != 0) {
            memcpy(dst, s_pipe_in + s_pipe_in_pos, n);
            s_pipe_in_pos += n;
            *got = n;
        }
        return VFS_OK;
    }
    if (s_in_fd >= 0) {
        return vfs_read(s_in_fd, dst, cap, got);
    }
    return VFS_OK;
}

static void cmd_cat(int argc, char** argv)
{
    int fd = -1;
    int rc = VFS_OK;

    // Без аргумента cat читает stdin - это единственное, ради чего нужен `<`
    // и что делает конвейер осмысленным.
    const bool from_stdin = (argc < 2);

    if (from_stdin) {
        if (!stdin_ready()) {
            out_diag("cat <файл>  или  cat < файл  или  вывод слева от |\n");
            return;
        }
    } else {
        // Тот же файл, что сейчас открыт на запись (`cat /a > /a`): прочитать
        // его уже не выйдет - точнее, выйдет пустоту. Отказываемся до открытия.
        if (sink_is_input(argv[1])) {
            out_diag("cat: этот файл сейчас перенаправлен на вывод\n");
            sink_cancel();
            return;
        }
        rc = vfs_open(argv[1], VFS_O_RDONLY, &fd);
        if (rc != VFS_OK) {
            out_path_error(rc);
            return;
        }
    }

    // Свой декодер на команду: он живёт между кусками чтения, иначе буква на
    // границе двух чтений рассыпалась бы. Через out_cp, а не out_puts: файл -
    // это байты, а не C-строка, нулей внутри него быть не может.
    //
    // В файл и в конвейер байты идут **как есть**. Разбирать UTF-8 на выходе
    // значило бы терять всё, что не является корректной последовательностью:
    // `cat /big > /copy` (где /big - не текст) давал файл заметно короче
    // исходного, и заметить это можно было только по размеру. Декодер нужен
    // ровно одному приёмнику - экранной матрице, которая знает только code point.
    const bool raw = (s_sink != SINK_SCREEN);

    utf8_decoder dec;
    utf8_reset(&dec);
    bool any = false;
    uint8_t last = '\n';

    for (;;) {
        uint32_t got = 0;
        rc = from_stdin ? stdin_read(s_cat_buf, sizeof(s_cat_buf), &got)
                        : vfs_read(fd, s_cat_buf, sizeof(s_cat_buf), &got);
        if (rc != VFS_OK) {
            break;
        }
        if (got == 0) {
            break;
        }

        any = true;
        last = s_cat_buf[got - 1];

        if (raw) {
            sink_bytes(s_cat_buf, got);
            continue;
        }

        for (uint32_t i = 0; i < got; ++i) {
            uint32_t cp = 0;
            if (utf8_feed(&dec, s_cat_buf[i], &cp) == UTF8_DONE) {
                out_cp(cp);
            }
        }
    }
    if (fd >= 0) {
        vfs_close(fd);
    }

    if (rc != VFS_OK) {
        out_path_error(rc);
        return;
    }

    // Недостающий перевод строки добавляем только на экране: иначе приглашение
    // прилипло бы к последней строке файла. В файл и конвейер лишний байт не
    // пишем - `cat` обязан копировать один в один.
    if (!raw && (!any || last != '\n')) {
        out_puts("\n");
    }
}

static void cmd_cd(int argc, char** argv)
{
    const int rc = vfs_chdir((argc > 1) ? argv[1] : "/");
    if (rc != VFS_OK) {
        out_path_error(rc);
    }
}

static void cmd_pwd(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    out_puts(vfs_cwd());
    out_puts("\n");
}

static void cmd_stat(int argc, char** argv)
{
    if (argc < 2) {
        out_diag("stat <путь>\n");
        return;
    }

    vfs_stat_t st;
    const int rc = vfs_stat(argv[1], &st);
    if (rc != VFS_OK) {
        out_path_error(rc);
        return;
    }

    out_puts(st.type == VFS_TYPE_DIR ? "каталог, " : "файл, ");
    out_bytes(st.size);
    if (st.mtime != 0) {
        char when[CAL_TEXT_MAX];
        out_puts(", изменён ");
        out_puts(cal_text(when, sizeof(when), st.mtime));
    } else {
        out_puts(", время изменения неизвестно");
    }
    out_puts("\n");
}

static void cmd_mkdir(int argc, char** argv)
{
    if (argc < 2) {
        out_diag("mkdir <каталог>\n");
        return;
    }
    const int rc = vfs_mkdir(argv[1]);
    if (rc != VFS_OK) {
        out_path_error(rc);
    }
}

static void cmd_rm(int argc, char** argv)
{
    if (argc < 2) {
        out_diag("rm <файл>\n");
        return;
    }

    const int rc = vfs_unlink(argv[1]);
    if (rc == VFS_ERR_IS_DIR) {
        // Отдельной строкой: «это каталог» без подсказки заставляет гадать,
        // чем его удалять.
        out_diag("это каталог: rmdir ");
        out_diag(argv[1]);
        out_diag("\n");
        return;
    }
    if (rc != VFS_OK) {
        out_path_error(rc);
    }
}

static void cmd_rmdir(int argc, char** argv)
{
    if (argc < 2) {
        out_diag("rmdir <каталог>\n");
        return;
    }
    const int rc = vfs_rmdir(argv[1]);
    if (rc != VFS_OK) {
        out_path_error(rc);
    }
}

// Те же 4 КБ, что и блок BFS: один вызов записи - один коммит ФС, поэтому
// мельче буфер означал бы больше коммитов на тот же файл и больше времени.
static uint8_t s_cp_buf[4096];

static void cmd_cp(int argc, char** argv)
{
    if (argc < 3) {
        out_diag("cp <откуда> <куда>\n");
        return;
    }

    // Копирование файла в себя сначала обнулило бы его (O_TRUNC), а потом
    // прочитало пустоту. Такое лучше отвергнуть сразу, чем объяснять
    // пропавшие данные.
    char from[VFS_PATH_MAX];
    char to[VFS_PATH_MAX];
    if (vfs_resolve(argv[1], from, sizeof(from)) == VFS_OK &&
        vfs_resolve(argv[2], to, sizeof(to)) == VFS_OK && strcmp(from, to) == 0) {
        out_diag("cp: откуда и куда - один и тот же файл\n");
        return;
    }

    int src = -1;
    int rc = vfs_open(argv[1], VFS_O_RDONLY, &src);
    if (rc != VFS_OK) {
        out_path_error(rc);
        return;
    }

    int dst = -1;
    rc = vfs_open(argv[2], VFS_O_CREATE | VFS_O_TRUNC | VFS_O_WRONLY, &dst);
    if (rc != VFS_OK) {
        vfs_close(src);
        out_path_error(rc);
        return;
    }

    for (;;) {
        uint32_t got = 0;
        rc = vfs_read(src, s_cp_buf, sizeof(s_cp_buf), &got);
        if (rc != VFS_OK || got == 0) {
            break;
        }

        uint32_t put = 0;
        rc = vfs_write(dst, s_cp_buf, got, &put);
        if (rc != VFS_OK) {
            break;
        }
        if (put != got) {
            // Меньше записалось - значит, место кончилось на середине, и
            // копия неполная. Молчать об этом нельзя.
            rc = VFS_ERR_NO_SPACE;
            break;
        }
    }

    vfs_close(dst);
    vfs_close(src);

    if (rc != VFS_OK) {
        out_path_error(rc);
    }
}

static void cmd_mv(int argc, char** argv)
{
    if (argc < 3) {
        out_diag("mv <откуда> <куда>\n");
        return;
    }

    const int rc = vfs_rename(argv[1], argv[2]);
    if (rc == VFS_ERR_ARG) {
        // За этой ошибкой стоит единственный случай, который в VFS запрещён
        // намеренно: переименование через границу точек монтирования.
        out_diag("mv: переименовать между разными точками монтирования нельзя\n");
        return;
    }
    if (rc != VFS_OK) {
        out_path_error(rc);
    }
}

static void cmd_df(int argc, char** argv)
{
    uint32_t used = 0;
    uint32_t total = 0;
    const int rc = vfs_usage((argc > 1) ? argv[1] : "/", &used, &total);
    if (rc != VFS_OK) {
        out_path_error(rc);
        return;
    }

    out_puts("занято ");
    out_kib(used);
    out_puts(" из ");
    out_kib(total);
    out_puts(" (");
    out_percent(used, total);
    out_puts("), свободно ");
    out_kib(total - used);
    out_puts("\n");
}

// --- rz: приём файла по XMODEM ---------------------------------------------
//
// Единственное место в шелле, которое читает консоль само: обычно байты
// забирает главный цикл и отдаёт их tty и редактору. Пока команда работает,
// цикл стоит в run_command, поэтому договорённость простая - команда владеет
// консолью целиком. Иначе байты протокола попали бы в строку и стали бы
// командой, а ACK ушёл бы в файл, если вывод перенаправлен.
//
// Запись копится в буфере размером с блок ФС: vfs_write на каждый килобайт
// пакета - это отдельная запись блока, и файл в сотню килобайт превратился бы
// в сотню мелких коммитов вместо двадцати пяти крупных.

#define RZ_BUF_SIZE 4096u

static uint8_t s_rz_buf[RZ_BUF_SIZE];

typedef struct {
    int fd;
    size_t len;
    uint32_t written;
} rz_state;

// Прогресс - только на экран. Поток USB занят протоколом: текст в нём
// отправитель обязан игнорировать, и проверять это на живом отправителе не
// стоит. Строка без перевода: следующее обновление затрёт её через \r.
static void rz_progress(uint32_t written)
{
    char num[21];
    text_puts("\rпринято ");
    text_puts(basalt_format_u64(num, sizeof(num), written));
    text_puts(" Б        ");
    present_dirty();
}

static bool rz_flush(rz_state* st)
{
    if (st->len == 0) {
        return true;
    }

    size_t done = 0;
    while (done < st->len) {
        uint32_t put = 0;
        const int rc = vfs_write(st->fd, s_rz_buf + done, (uint32_t)(st->len - done), &put);
        if (rc != VFS_OK || put == 0) {
            return false;
        }
        done += put;
    }

    st->written += (uint32_t)st->len;
    st->len = 0;
    rz_progress(st->written);
    return true;
}

static int rz_sink(void* ctx, const uint8_t* data, size_t len)
{
    rz_state* const st = ctx;

    while (len > 0) {
        const size_t room = RZ_BUF_SIZE - st->len;
        const size_t n = (len < room) ? len : room;
        memcpy(s_rz_buf + st->len, data, n);
        st->len += n;
        data += n;
        len -= n;

        if (st->len == RZ_BUF_SIZE && !rz_flush(st)) {
            return -1;
        }
    }
    return 0;
}

// Приёмник дошёл до EOT: дописываем буфер. XMODEM зовёт это до ACK, поэтому
// «передача прошла» и «файл записан» - одно и то же событие.
static int rz_finish(void* ctx)
{
    return rz_flush((rz_state*)ctx) ? 0 : -1;
}

static int rz_console_put(void* ctx, const uint8_t* data, size_t len)
{
    (void)ctx;
    return (int)basalt_console_write(data, len);
}

// Ожидание с общим сроком на весь вызов: get зовут один раз на пакет, и он
// сам решает, сколько ждать. Меньше len означает «срок вышел» - XMODEM
// ответит NAK и попросит пакет заново.
static int rz_console_get(void* ctx, uint8_t* data, size_t len, uint32_t timeout_ms)
{
    (void)ctx;

    const uint64_t deadline = basalt_uptime_us() + (uint64_t)timeout_ms * 1000u;
    size_t got = 0;

    for (;;) {
        got += basalt_console_read(data + got, len - got);
        if (got >= len || basalt_uptime_us() >= deadline) {
            break;
        }
        basalt_delay_us(100);
    }
    return (int)got;
}

static void rz_drain(void)
{
    uint8_t junk[64];
    while (basalt_console_read(junk, sizeof(junk)) > 0) {
    }
}

static void cmd_rz(int argc, char** argv)
{
    // Протокол идёт в консоль напрямую, мимо приёмников вывода. Перенаправление
    // вывода означало бы «ACK писать в файл», ввода - «пакеты читать из
    // файла»; и то и другое не передача, а недоразумение.
    //
    // Проверка раньше argc намеренно: у `rz > /x` аргументов нет, и ответ
    // «нужно имя файла» увёл бы от настоящей причины.
    if (s_sink != SINK_SCREEN || s_in_fd >= 0 || s_stdin_pipe) {
        out_diag("rz: перенаправление ввода-вывода не поддерживается\n");
        return;
    }

    if (argc < 2) {
        out_diag("rz: нужно имя файла\n");
        return;
    }

    if (!s_fs_ready) {
        out_diag("rz: файлы недоступны\n");
        return;
    }

    int fd = -1;
    int rc = vfs_open(argv[1], VFS_O_WRONLY | VFS_O_CREATE | VFS_O_TRUNC, &fd);
    if (rc != VFS_OK) {
        out_diag(argv[1]);
        out_diag(": ");
        out_fs_error(rc);
        out_diag("\n");
        return;
    }

    // Метка для tools/xsend.py: по ней отправитель понимает, что эхо набранной
    // команды кончилось и сейчас пойдёт протокол. Человеку с lrzsz она просто
    // говорит, что плата готова.
    out_diag("[rz] ждём XMODEM\n");

    // Всё, что успело прийти до приёма (эхо Enter, остатки ввода), выбрасываем:
    // первый байт пакета не должен оказаться чужой буквой.
    rz_drain();

    rz_state st;
    memset(&st, 0, sizeof(st));
    st.fd = fd;

    xmodem_io io;
    io.ctx = &st;
    io.put = rz_console_put;
    io.get = rz_console_get;
    io.sink = rz_sink;
    io.finish = rz_finish;

    const uint64_t started_us = basalt_uptime_us();
    uint32_t size = 0;
    rc = xmodem_receive(&io, &size);

    // Буфер дописан внутри приёма (rz_finish), поэтому закрывать можно сразу:
    // на диске уже всё, что обещал ACK.
    vfs_close(fd);

    // Строка прогресса осталась без перевода строки.
    text_putc('\n');

    if (rc != XMODEM_OK) {
        // Недописанный файл хуже отсутствующего: он выглядит как целый, и
        // отличить его от целого можно только по размеру, который никто не
        // помнит. Поэтому после ошибки удаляем - а если не удалось, говорим.
        const int un = vfs_unlink(argv[1]);

        out_diag("rz: ");
        out_diag(xmodem_strerror(rc));
        if (un != VFS_OK) {
            out_diag("; частичный файл остался: ");
            out_diag(vfs_strerror(un));
        }
        out_diag("\n");
        return;
    }

    out_diag("принято ");
    out_bytes(size);
    out_diag(" за ");
    out_u64((basalt_uptime_us() - started_us) / 1000u);
    out_diag(" мс\n");
}

// --- hexdump: файл байтами ---------------------------------------------------
//
// Читает файл кусками по строке дампа и печатает через out_puts, поэтому
// работает и с `>`, и с `|`: `hexdump /etc/rc | less` - обычное дело.
//
// Без аргумента читает stdin, как `cat`: иначе `hexdump < /etc/rc` и
// `hexdump /etc/rc | less` разошлись бы по смыслу, хотя оба читают один файл.
//
// Смещение считается по всему файлу, а не по куску: иначе на каждой новой
// строке чтения адрес начинался бы с нуля, и по дампу нельзя было бы понять,
// где что лежит.
static void cmd_hexdump(int argc, char** argv)
{
    const bool from_stdin = (argc < 2);
    if (from_stdin && !stdin_ready()) {
        out_diag("hexdump <файл>\n");
        return;
    }

    int fd = -1;
    if (!from_stdin) {
        const int rc = vfs_open(argv[1], VFS_O_RDONLY, &fd);
        if (rc != VFS_OK) {
            out_path_error(rc);
            return;
        }
    }

    uint8_t chunk[HEXDUMP_BYTES];
    char line[HEXDUMP_LINE_MAX];
    uint32_t offset = 0;
    int rc = VFS_OK;

    // Конец файла - это ноль прочитанных байт, а не «прочитали меньше, чем
    // просили»: короткое чтение внутри файла возможно у другой ФС, и обрывать
    // на нём дамп значило бы молча показать половину файла.
    for (;;) {
        uint32_t got = 0;
        rc = from_stdin ? stdin_read(chunk, sizeof(chunk), &got)
                        : vfs_read(fd, chunk, sizeof(chunk), &got);
        if (rc != VFS_OK || got == 0) {
            break;
        }

        // Буфер строки ровно того размера, который требует hexdump_line,
        // поэтому здесь она нуля не вернёт (ноль - это отказ, а не пустая
        // строка).
        hexdump_line(line, sizeof(line), offset, chunk, got);
        out_puts(line);
        offset += got;
    }

    if (!from_stdin) {
        vfs_close(fd);
    }

    if (rc != VFS_OK) {
        out_path_error(rc);
    }
}

// --- less: постраничный просмотр --------------------------------------------
//
// Вторая команда, которая владеет консолью и экраном (первая - `rz`): пока она
// работает, главный цикл стоит в run_command и байты в редактор не попадают.
// Иначе пробел, нажатый для листания, оказался бы в строке приглашения.
//
// Разметка страницы живёт в shell/pager.c и проверена на маке: здесь остаются
// только клавиши, экран и переходы по потоку.
//
// Экран после выхода очищается: страница файла не должна остаться висеть под
// приглашением, иначе следующая команда печатается поверх чужого текста.
// Приглашение и остальную картинку шелла рисует уже вызывающий.

static uint16_t s_less_cells[PAGER_ROWS][PAGER_COLS];

// Страница и экран - одно и то же, и расхождение видно не как ошибка сборки, а
// как «последняя строка страницы уехала под состояние». Проверяем числом, а не
// комментарием: смена геометрии матрицы должна ломать сборку, а не `less`.
_Static_assert(PAGER_COLS == TEXT_COLS && PAGER_ROWS == TEXT_ROWS - 1,
               "страница less обязана совпадать с экраном без строки состояния");

// Смещения показанных страниц для кнопки «назад», кольцом. Кольцо, а не
// растущий список: листать назад глубже 256 страниц не станут, а запись
// страницы `cur - 1` перезаписывается только через 256 страниц вперёд, то есть
// уже после того, как от неё ушли. Растущий список на 14 МБ файла - это 8700
// смещений по 4 байта, и он упёрся бы в память ради того, чего никто не делает.
#define LESS_HISTORY 256u
static uint32_t s_less_starts[LESS_HISTORY];

typedef struct {
    int fd;          // -1 - источник stdin (конвейер или `< файл`)
    bool seekable;   // можно ли вернуться на предыдущую страницу
} less_src;

// Чтение источника. Имя файла открыто дескриптором, stdin - общим путём
// (конвейер в памяти или `s_in_fd` после `<`), чтобы не заводить второй
// разбор того же случая.
static int less_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* got)
{
    less_src* const src = ctx;
    if (src->fd >= 0) {
        return vfs_read(src->fd, dst, cap, got);
    }
    return stdin_read(dst, cap, got);
}

static int less_seek(void* ctx, uint32_t offset)
{
    less_src* const src = ctx;
    if (!src->seekable || src->fd < 0) {
        return -1;
    }
    return vfs_seek(src->fd, offset);
}

// Клавиша с ожиданием. Возвращает байт нажатия как есть: разбирать его -
// дело команды, а не ожидания. Так читают обе полноэкранные команды - `less`
// и `edit`.
//
// flush - выбросить то, что уже накопилось, и только потом ждать. Это нужно
// один раз, на входе в полноэкранный режим: после Enter и эха команды в буфере
// остаётся перевод строки, и он прокрутил бы страницу без нажатия. Дальше
// выбрасывать нельзя: в редакторе накопившиеся байты - это набранные буквы, и
// «защита от застрявшего Enter» съедала бы их по восемь за нажатие.
static int console_key_ex(bool flush)
{
    uint8_t b = 0;

    if (flush) {
        for (int i = 0; i < 8; ++i) {
            if (basalt_console_read(&b, 1) == 0) {
                break;
            }
            basalt_delay_us(1000);
        }
    }

    for (;;) {
        if (basalt_console_read(&b, 1) > 0) {
            return b;
        }

        // Кнопка G0 - Ctrl-C: терминал на маке иногда держит прерывание у
        // себя, а на плате кнопка есть всегда. Байт именно 0x03, а не буква
        // `q`: редактор считает `q` обычным символом, и кнопка вставила бы её
        // в текст.
        if (button_down(BUTTON_PRIMARY)) {
            basalt_delay_us(30000);
            if (button_down(BUTTON_PRIMARY)) {
                return 0x03;
            }
        }
        basalt_delay_us(2000);
    }
}

// Обычное чтение для полноэкранных команд: накопленный хвост ввода не нужен.
static int console_key(void)
{
    return console_key_ex(true);
}

// Нарисовать страницу и строку состояния под ней. Курсор убираем: подчёркивание
// посреди файла читается как ещё один символ.
//
// can_back - у источника есть seek. От этого зависит подсказка: обещать `b`
// там, где назад нельзя, значит показать клавишу, которая выбросит из просмотра.
static void less_draw(const pager_page* page, bool can_back)
{
    text_clear();

    for (int r = 0; r < page->rows && r < PAGER_ROWS; ++r) {
        text_cursor_at(0, r);
        for (int c = 0; c < PAGER_COLS; ++c) {
            const uint16_t cp = s_less_cells[r][c];
            if (cp == 0) {
                break;
            }
            screen_put_cp(cp);
        }
    }

    // Строка состояния - в последней строке экрана. Без неё не видно ни того,
    // что файл длиннее страницы, ни того, что дальше уже конец.
    text_cursor_at(0, TEXT_ROWS - 1);
    if (page->eof) {
        text_puts(can_back ? "конец файла  b - назад, q - выход"
                           : "конец файла  q - выход");
    } else {
        text_puts(can_back ? "пробел/Enter - дальше, b - назад, q - выход"
                           : "пробел/Enter - дальше, q - выход");
    }

    // В USB-поток страница уходит построчно, как обычный вывод, а не как экран:
    // экрана в терминале мака нет, а текст есть - и прочитать файл в `pio
    // device monitor` так же естественно, как в `cat`. Стирать прошлую
    // страницу при этом нечем, и она остаётся в прокрутке - это цена за то,
    // что поток остаётся потоком.
    if (s_mirror) {
        char buf[4];
        for (int r = 0; r < page->rows && r < PAGER_ROWS; ++r) {
            for (int c = 0; c < PAGER_COLS; ++c) {
                const uint16_t cp = s_less_cells[r][c];
                if (cp == 0) {
                    break;
                }
                basalt_console_write(buf, utf8_encode(cp, buf));
            }
            mirror_str("\r\n");
        }
        mirror_str(page->eof ? "\r\n-- конец --\r\n" : "\r\n-- дальше --\r\n");
    }

    text_cursor_blink(false);
    present_dirty();
}

static void cmd_less(int argc, char** argv)
{
    // Как у rz: перенаправление вывода означало бы «страница ушла в файл».
    if (s_sink != SINK_SCREEN) {
        out_diag("less: перенаправление вывода не поддерживается\n");
        return;
    }
    if (argc < 2 && !stdin_ready()) {
        out_diag("less <файл>\n");
        return;
    }

    less_src src;
    memset(&src, 0, sizeof(src));
    src.fd = -1;

    // Источник: имя файла либо stdin (`< файл` и конвейер). Назад можно только
    // у файла: у конвейера нет ни дескриптора, ни смещения.
    bool own_fd = false;
    if (argc >= 2) {
        const int rc = vfs_open(argv[1], VFS_O_RDONLY, &src.fd);
        if (rc != VFS_OK) {
            out_path_error(rc);
            return;
        }
        src.seekable = true;
        own_fd = true;
    } else if (s_in_fd >= 0) {
        src.fd = s_in_fd;
        src.seekable = true;
    }

    pager pg;
    pager_io io;
    io.ctx = &src;
    io.read = less_read;
    io.seek = less_seek;
    pager_init(&pg, io);

    // Смещение страницы, которую надо показать. Единственный источник истины:
    // «назад» и «вперёд» меняют только его, а чтение каждый раз начинается с
    // него же. Иначе после возврата на первую страницу поток оставался бы на
    // второй, и следующая страница показала бы текст не с того места.
    uint32_t want = 0;
    uint32_t cur = 0;  // номер показанной страницы, для кольца смещений

    // Чем кончился просмотр, если кончился ошибкой. Печатать сразу нельзя:
    // на выходе экран чистится, и сообщение исчезло бы вместе с ним - на плате
    // от него не осталось бы даже следа.
    const char* fault = NULL;
    int fault_rc = VFS_OK;

    for (;;) {
        // seek на каждой странице, а не только при возврате: он стоит одну
        // установку смещения в VFS, а раздвоенный путь «сейчас я после
        // прокрутки» / «сейчас я после возврата» уже один раз показал не ту
        // страницу.
        if (src.seekable && pager_seek(&pg, want) != 0) {
            fault = "less: поток не прокручивается";
            break;
        }

        const pager_page page = pager_fill(&pg, s_less_cells);

        if (pg.error != VFS_OK) {
            fault_rc = pg.error;
            break;
        }

        less_draw(&page, src.seekable && cur > 0);

        // Смещение текущей страницы записывается и на последней: иначе с конца
        // файла нельзя было бы вернуться назад, а «конец» - не повод терять
        // место, где читатель остановился.
        s_less_starts[cur % LESS_HISTORY] = want;

        int key;
        if (page.eof) {
            // «Конец» не должен исчезать сам: последние строки читают глазами.
            // И уйти с него назад тоже должно быть можно - файл не кончился
            // там, где перестал влезать на экран. А если назад некуда (одна
            // страница) или нечем (конвейер), `b` просто не значит ничего:
            // выбрасывать читателя с последней строки из-за лишнего нажатия
            // незачем.
            const bool can_back = src.seekable && cur > 0;
            do {
                key = console_key();
            } while (key != 'q' && key != 0x03 && !(key == 'b' && can_back));
        } else {
            key = console_key();
        }

        if (key == 'q' || key == 0x03) {
            break;
        }

        if (key == 'b') {
            // Возврат на первую страницу - это повторное чтение её же с
            // начала, а не «остаться где стоим»: поток уже ушёл вперёд.
            if (!src.seekable) {
                // У конвейера смещений нет: перечитать предыдущую страницу
                // нечем, а показать следующую под видом предыдущей - соврать.
                fault = "less: назад нельзя, поток не прокручивается";
                break;
            }
            if (cur > 0) {
                --cur;
                want = s_less_starts[cur % LESS_HISTORY];
            }
            continue;
        }

        // На конце файла вперёд некуда: искать страницу за концом значило бы
        // показать пустую и потребовать ещё одно нажатие.
        if (page.eof) {
            continue;
        }

        // Пробел, Enter и всё остальное - вперёд. У обычного `less` пробел
        // листает страницу, Enter - строку; здесь страница, и это сказано в
        // строке состояния.
        want = page.next;
        ++cur;
    }

    if (own_fd) {
        vfs_close(src.fd);
    }

    text_clear();

    // Сообщение - после очистки, уже поверх приглашения: экран больше не наш.
    if (fault_rc != VFS_OK) {
        out_path_error(fault_rc);
    } else if (fault != NULL) {
        out_diag(fault);
        out_diag("\n");
    }
}

// --- edit: полноэкранный редактор -------------------------------------------
//
// Третья команда, которая владеет консолью и экраном (после `rz` и `less`):
// клавиши читает сама, поэтому перенаправление для неё - ошибка, а не режим.
//
// Текст, курсор и клавиши живут в shell/editor.c и проверены на маке: здесь
// остаются файл, экран, сохранение и строка состояния.
//
// На маке включается альтернативный экран (CSI ?1049), чтобы после выхода
// вернулся прежний текст терминала; на плате матрица просто чистится. Строки
// перерисовываются только изменившиеся: полная перерисовка на каждое нажатие -
// это 16 полос по 6 КБ через SPI.

// 64 КБ - предел на файл. Редактор держит текст целиком, а не листает файл:
// вставка в середину требует сдвига хвоста, и без буфера пришлось бы
// переписывать файл на каждую букву. Для файлов с мака этого хватает, а
// больше - это уже про другой редактор.
#define EDIT_BUF_SIZE (64u * 1024u)

static uint16_t s_edit_cells[EDITOR_ROWS][EDITOR_COLS];

// Что было на экране в прошлый раз. Сравнение с ним и есть «перерисовать
// только изменившиеся строки»; флагов изменённости матрицы для этого мало -
// она помечает строку при каждой записи, даже если текст тот же.
static uint16_t s_edit_prev[EDITOR_ROWS][EDITOR_COLS];

// Строка состояния и сообщения о сохранении. Статикой, а не на стеке: текст
// рисуется на следующей итерации, когда локальная переменная уже мертва.
static char s_edit_msg[96];

_Static_assert(EDITOR_COLS == TEXT_COLS && EDITOR_ROWS == TEXT_ROWS - 1,
               "окно редактора обязано совпадать с экраном без строки состояния");

static size_t str_app(char* out, size_t cap, size_t at, const char* s)
{
    while (*s != '\0' && at + 1 < cap) {
        out[at++] = *s++;
    }
    out[at] = '\0';
    return at;
}

// Сколько байт строки влезает в max_cells клеток.
static size_t cells_prefix_len(const char* s, int max_cells)
{
    utf8_decoder dec;
    utf8_reset(&dec);

    int cells = 0;
    size_t at = 0;
    while (s[at] != '\0' && cells < max_cells) {
        uint32_t cp = 0;
        const utf8_status st = utf8_feed(&dec, (uint8_t)s[at], &cp);
        ++at;
        if (st != UTF8_PENDING) {
            ++cells;  // и настоящий символ, и '?' на месте битого - клетка
        }
    }
    return at;
}

// Вывести не больше max_cells клеток. Возвращает, сколько вывел: по этому
// числу ставится курсор в строке ввода.
static int put_cells(const char* s, int max_cells)
{
    utf8_decoder dec;
    utf8_reset(&dec);

    int cells = 0;
    for (const char* p = s; *p != '\0' && cells < max_cells; ++p) {
        uint32_t cp = 0;
        const utf8_status st = utf8_feed(&dec, (uint8_t)*p, &cp);
        if (st == UTF8_PENDING) {
            continue;
        }
        if (st == UTF8_INVALID) {
            cp = '?';
        }
        screen_put_cp(cp);
        ++cells;
    }
    return cells;
}

// То же, но с добивкой пробелами до конца строки. Добивать обязательно: строка
// состояния меняется от длинной подсказки к короткому сообщению, и в USB хвост
// стирает ESC[K, а в матрице стирать некому - старый текст остался бы висеть
// справа. Возвращается число клеток текста, а не добивки.
static int put_cells_pad(const char* s, int max_cells)
{
    const int cells = put_cells(s, max_cells);
    for (int c = cells; c < max_cells; ++c) {
        screen_put_cp(' ');
    }
    return cells;
}

// Поставить курсор терминала мака в строку и колонку. Настоящий терминал
// умеет это сам, поэтому в USB уходят байты позиционирования, а не полная
// картинка: на 115200 полный экран - это треть секунды на каждое нажатие.
static void mirror_at(int row, int col)
{
    if (!s_mirror) {
        return;
    }
    char num[21];
    mirror_str("\x1b[");
    mirror_str(basalt_format_u64(num, sizeof(num), (uint64_t)row + 1));
    mirror_str(";");
    mirror_str(basalt_format_u64(num, sizeof(num), (uint64_t)col + 1));
    mirror_str("H");
}

static void mirror_cells(const uint16_t* cells, int n)
{
    if (!s_mirror) {
        return;
    }
    char buf[4];
    for (int c = 0; c < n; ++c) {
        const size_t k = utf8_encode(cells[c], buf);
        if (k > 0) {
            basalt_console_write(buf, k);
        }
    }
    mirror_str("\x1b[K");
}

// Строка состояния: имя, позиция курсора, признак правок и сообщение.
static void edit_status_line(char* out, size_t cap, const editor* ed, const char* name,
                             const char* msg)
{
    uint32_t line = 0;
    uint32_t col = 0;
    ed_line_col(ed, &line, &col);

    char num[21];
    size_t at = 0;
    at = str_app(out, cap, at, name);
    at = str_app(out, cap, at, "  ");
    at = str_app(out, cap, at, basalt_format_u64(num, sizeof(num), line));
    at = str_app(out, cap, at, ":");
    at = str_app(out, cap, at, basalt_format_u64(num, sizeof(num), col));
    if (ed_dirty(ed)) {
        // Звёздочка, как в vim: несохранённое должно быть видно, не открывая
        // заново меню.
        at = str_app(out, cap, at, " *");
    }
    if (msg != NULL && msg[0] != '\0') {
        at = str_app(out, cap, at, "  ");
        (void)str_app(out, cap, at, msg);
    }
}

static void edit_draw(const editor* ed, const char* name, const char* msg, bool force)
{
    for (int r = 0; r < EDITOR_ROWS; ++r) {
        const int n = ed_row(ed, r, s_edit_cells[r], EDITOR_COLS);
        for (int c = n; c < EDITOR_COLS; ++c) {
            s_edit_cells[r][c] = ' ';
        }

        if (!force && memcmp(s_edit_cells[r], s_edit_prev[r], sizeof(s_edit_cells[r])) == 0) {
            continue;
        }
        memcpy(s_edit_prev[r], s_edit_cells[r], sizeof(s_edit_cells[r]));

        text_cursor_at(0, r);
        for (int c = 0; c < EDITOR_COLS; ++c) {
            screen_put_cp(s_edit_cells[r][c]);
        }

        mirror_at(r, 0);
        mirror_cells(s_edit_cells[r], EDITOR_COLS);
    }

    char line[TEXT_COLS * 4];
    edit_status_line(line, sizeof(line), ed, name, msg);

    // Не больше TEXT_COLS - 1 клеток: 53-я вызвала бы перевод строки, а на
    // последней строке экрана это прокрутка всего экрана - редактор «поехал»
    // бы вверх на каждое нажатие.
    text_cursor_at(0, TEXT_ROWS - 1);
    (void)put_cells_pad(line, TEXT_COLS - 1);

    if (s_mirror) {
        mirror_at(TEXT_ROWS - 1, 0);
        basalt_console_write(line, cells_prefix_len(line, TEXT_COLS - 1));
        mirror_str("\x1b[K");
    }

    int row = 0;
    int col = 0;
    ed_cursor_screen(ed, &row, &col);
    text_cursor_at(col, row);
    text_cursor_blink(true);

    // Курсор терминала - в его же координатах, а не в матричных: строк
    // состояния на маке нет, а экран редактора начинается с первой строки.
    mirror_at(row, col);
    present_dirty();
}

// Имя временного файла рядом с целевым. BFS_NAME_MAX = 24, поэтому длинное
// имя с суффиксом не влезет - тогда в том же каталоге берётся .edit.tmp.
// Потерять безопасность записи из-за длины имени хуже редкой коллизии:
// редактор один, второй записи в тот же каталог одновременно не будет.
static bool edit_temp_path(const char* path, char* out, size_t cap)
{
    const char* const slash = strrchr(path, '/');
    const size_t dir = (slash == NULL) ? 0 : (size_t)(slash - path) + 1;
    const char* const leaf = (slash == NULL) ? path : slash + 1;

    const bool fits = (strlen(leaf) + 4 <= BFS_NAME_MAX);
    const char* const suffix = fits ? ".tmp" : ".edit.tmp";
    const size_t keep = fits ? strlen(leaf) : 0;

    const size_t total = dir + keep + strlen(suffix);
    if (total + 1 > cap) {
        return false;
    }
    memcpy(out, path, dir + keep);
    strcpy(out + dir + keep, suffix);
    return true;
}

// Записать текст и заменить им файл. Атомарной замены в BFS нет: rename
// отказывает, если цель существует. Порядок поэтому такой: сначала целиком
// пишется .tmp, и только потом старый файл удаляется и .tmp переименовывается.
// Сбой посреди записи оставит целый старый файл и мусорный .tmp; сбой между
// удалением и переименованием - только .tmp, и он целый.
static int edit_save(const char* path, const char* data, uint32_t len, char* tmp, size_t tmp_cap)
{
    if (!edit_temp_path(path, tmp, tmp_cap)) {
        return VFS_ERR_NAME;
    }

    int fd = -1;
    int rc = vfs_open(tmp, VFS_O_WRONLY | VFS_O_CREATE | VFS_O_TRUNC, &fd);
    if (rc != VFS_OK) {
        return rc;
    }

    uint32_t done = 0;
    while (done < len) {
        uint32_t put = 0;
        rc = vfs_write(fd, data + done, len - done, &put);
        if (rc != VFS_OK || put == 0) {
            break;
        }
        done += put;
    }
    vfs_close(fd);

    if (rc != VFS_OK) {
        vfs_unlink(tmp);
        return rc;
    }

    rc = vfs_unlink(path);
    if (rc != VFS_OK && rc != VFS_ERR_NOT_FOUND) {
        vfs_unlink(tmp);
        return rc;
    }

    return vfs_rename(tmp, path);
}

// Строка поиска рисуется в той же строке состояния. Отдельный мини-редактор
// ради одного поля - лишний код: нужны набор, Backspace и Enter.
static void edit_ask_draw(const char* prompt, const char* text)
{
    char line[TEXT_COLS * 4];
    const size_t at = str_app(line, sizeof(line), 0, prompt);
    (void)str_app(line, sizeof(line), at, text);

    text_cursor_at(0, TEXT_ROWS - 1);
    const int cells = put_cells_pad(line, TEXT_COLS - 1);

    if (s_mirror) {
        mirror_at(TEXT_ROWS - 1, 0);
        basalt_console_write(line, cells_prefix_len(line, TEXT_COLS - 1));
        mirror_str("\x1b[K");
        mirror_at(TEXT_ROWS - 1, cells);
    }

    text_cursor_at(cells < TEXT_COLS ? cells : TEXT_COLS - 1, TEXT_ROWS - 1);
    text_cursor_blink(true);
    present_dirty();
}

// true - строка введена, false - отменено (Ctrl-C, Ctrl-Q, ESC).
static bool edit_ask(const char* prompt, char* out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';

    utf8_decoder dec;
    utf8_reset(&dec);

    // Хвост ввода от команды выбрасываем один раз, на первом нажатии: дальше
    // это уже набранные буквы.
    bool first = true;
    for (;;) {
        edit_ask_draw(prompt, out);

        const int key = console_key_ex(first);
        first = false;
        if (key == 0x03 || key == 0x11 || key == 0x1B) {
            return false;
        }
        if (key == '\r' || key == '\n') {
            return true;
        }
        if (key == 0x7F || key == 0x08) {
            // Откатываем целый символ, а не байт: «Ж» - два байта.
            while (len > 0 && ((uint8_t)out[len - 1] & 0xC0u) == 0x80u) {
                --len;
            }
            if (len > 0) {
                --len;
            }
            out[len] = '\0';
            utf8_reset(&dec);
            continue;
        }
        if (key < 0x20) {
            continue;
        }

        uint32_t cp = 0;
        if (utf8_feed(&dec, (uint8_t)key, &cp) != UTF8_DONE) {
            continue;
        }
        char enc[4];
        const size_t n = utf8_encode(cp, enc);
        if (n > 0 && len + n + 1 <= cap) {
            memcpy(out + len, enc, n);
            len += n;
            out[len] = '\0';
        }
    }
}

static void cmd_edit(int argc, char** argv)
{
    // Как у rz и less: перенаправление вывода означало бы «экран ушёл в файл».
    if (s_sink != SINK_SCREEN) {
        out_diag("edit: перенаправление вывода не поддерживается\n");
        return;
    }
    if (argc < 2) {
        out_diag("edit <файл>\n");
        return;
    }

    // Текст - в PSRAM: это буфер под данные, а не горячий код. Если PSRAM на
    // плате нет, годится и обычная память, как у конвейера.
    char* buf = heap_caps_malloc(EDIT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(EDIT_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        out_diag("edit: нет памяти под буфер\n");
        return;
    }

    editor ed;
    ed_init(&ed, buf, EDIT_BUF_SIZE);

    // Нет файла - это новый файл, а не ошибка: `edit notes.txt` должен
    // открывать пустой буфер, иначе набрать первый текст было бы нечем.
    uint32_t len = 0;
    bool truncated = false;
    int fd = -1;
    int read_rc = VFS_OK;

    const int open_rc = vfs_open(argv[1], VFS_O_RDONLY, &fd);
    if (open_rc == VFS_OK) {
        for (;;) {
            uint32_t got = 0;
            read_rc = vfs_read(fd, buf + len, EDIT_BUF_SIZE - len, &got);
            if (read_rc != VFS_OK || got == 0) {
                break;
            }
            len += got;

            if (len == EDIT_BUF_SIZE) {
                // Ровно 64 КБ тоже бывает: проверяем, есть ли за ними ещё.
                uint8_t extra = 0;
                uint32_t more = 0;
                truncated = (vfs_read(fd, &extra, 1, &more) == VFS_OK && more > 0);
                break;
            }
        }
        vfs_close(fd);
    } else if (open_rc != VFS_ERR_NOT_FOUND) {
        out_path_error(open_rc);
        heap_caps_free(buf);
        return;
    }

    if (read_rc != VFS_OK) {
        out_path_error(read_rc);
        heap_caps_free(buf);
        return;
    }

    ed_load(&ed, len);

    if (s_mirror) {
        mirror_str("\x1b[?1049h");
        mirror_str("\x1b[H\x1b[2J");
    }

    text_clear();
    memset(s_edit_prev, 0, sizeof(s_edit_prev));

    str_app(s_edit_msg, sizeof(s_edit_msg), 0,
            truncated ? "файл больше 64 КБ - читаю начало, сохранить нельзя"
                      : "Ctrl-S сохранить, Ctrl-F найти, Ctrl-Q выход");

    const char* msg = s_edit_msg;
    bool force = true;
    bool first = true;

    for (;;) {
        edit_draw(&ed, argv[1], msg, force);
        force = false;
        msg = NULL;

        // Хвост ввода от команды - только на первом нажатии: дальше каждый
        // байт в буфере это буква, которую человек уже набрал.
        const int key = console_key_ex(first);
        first = false;
        const ed_event ev = ed_input(&ed, (uint8_t)key);

        if (ev == ED_QUIT) {
            break;
        }

        switch (ev) {
        case ED_SAVE: {
            // Файл прочитан не целиком: запись затёрла бы хвост, который в
            // буфер не влез. Молча терять данные нельзя, а «сохранить первые
            // 64 КБ» - это не сохранение.
            if (truncated) {
                str_app(s_edit_msg, sizeof(s_edit_msg), 0,
                        "файл больше 64 КБ - сохранить нельзя");
                msg = s_edit_msg;
                break;
            }

            char tmp[VFS_PATH_MAX];
            const int save_rc = edit_save(argv[1], ed_data(&ed), ed_len(&ed), tmp, sizeof(tmp));
            if (save_rc == VFS_OK) {
                ed_mark_saved(&ed);

                char num[21];
                size_t at = str_app(s_edit_msg, sizeof(s_edit_msg), 0, "сохранено, ");
                at = str_app(s_edit_msg, sizeof(s_edit_msg), at,
                             basalt_format_u64(num, sizeof(num), ed_len(&ed)));
                (void)str_app(s_edit_msg, sizeof(s_edit_msg), at, " Б");
            } else {
                // Причина - в строке состояния: напечатанное до очистки экрана
                // на плате не видно, и «не сохранилось» выглядело бы как
                // потерянный файл.
                const size_t at = str_app(s_edit_msg, sizeof(s_edit_msg), 0, "не сохранено: ");
                (void)str_app(s_edit_msg, sizeof(s_edit_msg), at, vfs_strerror(save_rc));
            }
            msg = s_edit_msg;
            break;
        }

        case ED_QUIT_DIRTY:
            str_app(s_edit_msg, sizeof(s_edit_msg), 0, "есть правки: Ctrl-Q ещё раз - выйти");
            msg = s_edit_msg;
            break;

        case ED_FIND: {
            char needle[64];
            if (edit_ask("поиск: ", needle, sizeof(needle)) && needle[0] != '\0') {
                if (ed_search(&ed, needle) != 0) {
                    str_app(s_edit_msg, sizeof(s_edit_msg), 0, "не найдено");
                    msg = s_edit_msg;
                }
            }
            break;
        }

        default:
            break;
        }
    }

    if (s_mirror) {
        mirror_str("\x1b[?1049l");
    }

    text_clear();
    text_cursor_blink(true);
    heap_caps_free(buf);
}

static const command s_commands[] = {
    {"help",    cmd_help,    "этот список"},
    {"echo",    cmd_echo,    "напечатать аргументы"},
    {"history", cmd_history, "последние команды"},
    {"date",    cmd_date,    "время; date set ГГГГ-ММ-ДД ЧЧ:ММ"},
    {"sleep",   cmd_sleep,   "подождать: 5, 300ms, 1m30s"},
    {"alarm",   cmd_alarm,   "будильник: мигание подсветкой"},
    {"free",    cmd_free,    "память и стек"},
    {"top",     cmd_top,     "задачи и остаток стека"},
    {"beep",    cmd_beep,    "писк в терминал"},
    {"clear",   cmd_clear,   "очистить экран"},
    {"uptime",  cmd_uptime,  "время работы"},
    {"tickrate", cmd_tickrate, "измерить частоту тика"},
    {"mirror",  cmd_mirror,  "on|off: повторять поток в USB"},
    {"reboot",  cmd_reboot,  "перезагрузка"},
    {"crash",   cmd_crash,   "проверить панику"},
    {"ls",      cmd_ls,      "каталог: ls -l — даты и размеры"},
    {"cat",     cmd_cat,     "показать файл"},
    {"hexdump", cmd_hexdump, "файл байтами: смещение, hex, ASCII"},
    {"less",    cmd_less,    "постранично: пробел, b, q"},
    {"edit",    cmd_edit,    "полноэкранный редактор"},
    {"cd",      cmd_cd,      "сменить каталог"},
    {"pwd",     cmd_pwd,     "текущий каталог"},
    {"stat",    cmd_stat,    "тип и размер"},
    {"mkdir",   cmd_mkdir,   "создать каталог"},
    {"rm",      cmd_rm,      "удалить файл"},
    {"rmdir",   cmd_rmdir,   "удалить пустой каталог"},
    {"cp",      cmd_cp,      "скопировать файл"},
    {"mv",      cmd_mv,      "переименовать"},
    {"df",      cmd_df,      "место в ФС"},
    {"rz",      cmd_rz,      "принять файл по XMODEM"},
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

static command_fn find_command(const char* name)
{
    for (size_t i = 0; i < s_command_count; ++i) {
        if (strcmp(name, s_commands[i].name) == 0) {
            return s_commands[i].fn;
        }
    }
    return NULL;
}

// Выполнить одну ступень: открыть `<` и `>`, выставить приёмник, позвать
// команду, всё вернуть назад. false - строку надо бросить (причина уже
// напечатана).
//
// Приёмник восстанавливается до печати ошибок намеренно: сообщение оболочки
// принадлежит экрану, а не файлу, в который команда не смогла писать.
static bool run_stage(const redirect_stage* st, bool capture, int slot)
{
    // Вход и выход - один файл: усечение при первой записи обрезало бы данные,
    // которые команда ещё читает. Проверяем до открытия, пока ничего не тронуто.
    if (st->in_path != NULL && st->out_path != NULL) {
        char from[VFS_PATH_MAX];
        char to[VFS_PATH_MAX];
        if (vfs_resolve(st->in_path, from, sizeof(from)) == VFS_OK &&
            vfs_resolve(st->out_path, to, sizeof(to)) == VFS_OK &&
            strcmp(from, to) == 0) {
            out_diag("?: вход и выход - один и тот же файл\n");
            return false;
        }
    }

    int in_fd = -1;
    if (st->in_path != NULL) {
        const int rc = vfs_open(st->in_path, VFS_O_RDONLY, &in_fd);
        if (rc != VFS_OK) {
            out_diag(st->in_path);
            out_diag(": ");
            out_diag(vfs_strerror(rc));
            out_diag("\n");
            return false;
        }
    }

    int out_fd = -1;
    if (st->out_path != NULL) {
        // Без O_TRUNC: усечение отложено до первой записи (см. s_out_trunc_pending).
        const uint8_t flags = (uint8_t)(VFS_O_WRONLY | VFS_O_CREATE |
                                        (st->append ? VFS_O_APPEND : 0));
        const int rc = vfs_open(st->out_path, flags, &out_fd);
        if (rc != VFS_OK) {
            if (in_fd >= 0) {
                vfs_close(in_fd);
            }
            out_diag(st->out_path);
            out_diag(": ");
            out_diag(vfs_strerror(rc));
            out_diag("\n");
            return false;
        }
    }

    const out_sink prev_sink = s_sink;
    const int prev_out = s_out_fd;
    s_out_fd = out_fd;
    s_out_failed = false;
    s_out_cancel = false;
    s_out_len = 0;
    s_out_path = st->out_path;
    s_out_trunc_pending = (out_fd >= 0 && !st->append);
    s_out_canon[0] = '\0';
    if (out_fd >= 0) {
        vfs_resolve(st->out_path, s_out_canon, sizeof(s_out_canon));
    }
    s_in_fd = in_fd;

    if (capture) {
        s_sink = SINK_PIPE;
        s_pipe_slot = slot;
        s_pipe_len = 0;
        s_pipe_overflow = false;
    } else {
        s_sink = (out_fd >= 0) ? SINK_FILE : SINK_SCREEN;
    }

    const command_fn fn = find_command(st->argv[0]);
    if (fn != NULL) {
        fn(st->argc, st->argv);
    }

    // Дописать остаток буфера и, если писать так и не начали, всё-таки
    // усечь файл: `> f` без вывода даёт пустой файл, как в sh.
    if (out_fd >= 0 && !s_out_cancel) {
        out_file_flush();
        if (s_out_trunc_pending) {
            s_out_trunc_pending = false;
            if (vfs_truncate(s_out_path, 0) != VFS_OK) {
                s_out_failed = true;
            }
        }
    }

    s_sink = prev_sink;
    s_out_fd = prev_out;
    s_out_len = 0;
    s_out_path = NULL;
    s_out_trunc_pending = false;
    s_out_canon[0] = '\0';
    s_in_fd = -1;

    if (out_fd >= 0) {
        vfs_close(out_fd);
    }
    if (in_fd >= 0) {
        vfs_close(in_fd);
    }

    if (fn == NULL) {
        out_diag("?: ");
        out_diag(st->argv[0]);
        out_diag(": нет такой команды, help - список\n");
        return false;
    }
    if (s_out_cancel) {
        return false;
    }
    if (s_out_failed) {
        out_diag("?: ");
        out_diag(st->out_path);
        out_diag(": запись не удалась, место кончилось?\n");
        return false;
    }
    if (capture && s_pipe_overflow) {
        char num[21];
        out_diag("?: конвейер: вывод не влез в ");
        out_diag(basalt_format_u64(num, sizeof(num), PIPE_BUF_SIZE / 1024u));
        out_diag(" КБ\n");
        return false;
    }
    return true;
}

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
        out_diag("?: кавычка не закрыта\n");
        return;

    case PARSE_ERR_ARGS:
        out_diag("?: слишком много слов\n");
        return;
    }

    if (args.argc == 0) {
        return;
    }

    redirect_plan plan;
    const redirect_status rs = redirect_build(&args, &plan);
    if (rs != REDIRECT_OK) {
        out_diag("?: ");
        out_diag(redirect_strerror(rs));
        out_diag("\n");
        return;
    }

    s_pipe_in = NULL;
    s_pipe_in_len = 0;
    s_pipe_in_pos = 0;

    for (int i = 0; i < plan.count; ++i) {
        const bool last = (i + 1 == plan.count);

        // Вход ступени - то, что накопила предыдущая. Флаг отдельно от
        // указателя: у пустого вывода буфера может не быть, но «вход есть,
        // он пустой» и «входа нет» - разные вещи.
        s_stdin_pipe = (i > 0);

        if (!run_stage(&plan.stage[i], !last, i % 2)) {
            break;
        }
        if (!last) {
            s_pipe_in = s_pipe[i % 2];
            s_pipe_in_len = s_pipe_len;
            s_pipe_in_pos = 0;
        }
    }

    s_stdin_pipe = false;
    s_pipe_in = NULL;
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

// --- задачи (веха A6, шаг 2) ----------------------------------------------
//
// Демонстрационная задача. Существует не «чтобы было», а чтобы переключение
// контекста было видно на плате, а не только в коде: она считает итерации и
// отдаёт процессор, а `top` показывает, что работа шла.
//
// Почему счётчик, а не печать в консоль. Печать из задачи конфликтовала бы с
// шеллом: оба пишут в одну матрицу и один USB. Счётчик же доказывает главное -
// задача действительно выполнялась, пока шелл ждал ввода.
static volatile uint32_t s_task_iterations;

static void demo_task(void* arg)
{
    (void)arg;

    for (;;) {
        // Работа задачи: считаем итерации. Именно это число должно расти в
        // `top`, пока шелл стоит на приглашении.
        s_task_iterations += 1;

        // Отдаём процессор. Без этой строки задача не уступила бы никогда, и
        // шелл перестал бы отвечать: кооперативная система держится на том,
        // что каждая задача сама уступает.
        basalt_task_yield();
    }
}

// Сколько итераций сделала демо-задача. Для `top`: если число не растёт,
// планировщик не работает, и остальные его показатели ничего не значат.
uint32_t shell_task_iterations(void)
{
    return s_task_iterations;
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
    fs_mount_root();
    fs_ensure_rc();

    // Часы - до /etc/rc: если в стартовом файле есть `ls -l` или `date`, они
    // должны видеть уже установленное время, а не прочерк.
    fs_load_clock();
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

    // /etc/rc выполняется до `ready`: его вывод - часть старта, а не ответ на
    // команду. Если файл пуст или его нет, строк тут не появится.
    fs_run_rc();

    // Таймерное прерывание включается последним, после /etc/rc и перед
    // ready: с этого момента любой код может быть прерван в любой
    // инструкции. Всё, что должно было пройти без помех - старт драйверов,
    // разбор конфигурации, вывод баннера - к этому моменту уже позади.
    // Проверяем это первым, до `ready`: если прерывание сломает систему,
    // последняя строка в логе укажет на него, а не на команду пользователя.
    basalt_int1_init();

    // Сон разрешается сразу после тика: до этого waiti уснул бы навсегда,
    // потому что разбудить его было бы некому.
    basalt_idle_init();

    // Планировщик - после тика и сна: до этого задача, отдавшая процессор,
    // разбудить себя не смогла бы. Главный поток становится задачей 0.
    basalt_task_init("главная");

    basalt_put("[shell] тик ");
    basalt_put_u64(BASALT_INT1_HZ);
    basalt_put(" Гц, ядро ");
    basalt_put_u64(basalt_int1_cpu_hz() / 1000000u);
    basalt_put(" МГц\n");

    basalt_put("ready\n\n");

    // Демо-задача заводится после ready: до этой строки порт на маке может
    // быть ещё не открыт, и всё, что она напечатала бы, ушло бы в никуда.
    //
    // Стек 4 КБ при BASALT_TASK_MAX=4: четыре задачи - это 16 КБ внутренней
    // памяти, которая у нас не бесконечна (см. top).
    const basalt_task_config_t demo = {
        .name = "пульс",
        .fn = demo_task,
        .arg = NULL,
        .stack_size = 0,  // BASALT_TASK_STACK_SIZE
    };
    if (!basalt_task_create(&demo)) {
        basalt_put("[shell] задача не создана: нет памяти\n");
    }

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

        // Отдаём процессор, если есть кому. Возвращённое true означает, что
        // переключение состоялось и кто-то поработал: тогда спать нельзя -
        // иначе отданная задача получит управление только через тик.
        if (basalt_task_yield()) {
            continue;
        }

        // Спим до ближайшего тика, а не крутимся на basalt_delay_us(500).
        //
        // Сон ограничен частотой тика: 10 мс. USB-прерывание у нас не
        // разрешено (консоль опрашивается), поэтому разбудить раньше тика
        // некому - и 10 мс это верхняя граница задержки отклика на клавишу.
        // Для терминала незаметно, а в простое ядро перестаёт выполнять
        // миллионы пустых итераций.
        //
        // Почему не «спать до следующего мигания курсора»: любое прерывание
        // возвращает управление, а условие перепроверяется сверху. Спать
        // ровно до срока бессмысленно - тик всё равно разбудит через 10 мс.
        basalt_idle_wait();
    }
}
