// Редактор строки. Зачем свой - см. lineedit.h.

#include "lineedit.h"

#include <string.h>

#include "history.h"
#include "utf8.h"

// Управляющие коды. Имена по назначению, а не по номеру: 0x01 читается хуже,
// чем CTRL_A, и при отладке не приходится помнить таблицу ASCII.
#define CTRL_A 0x01
#define CTRL_C 0x03
#define CTRL_D 0x04
#define CTRL_E 0x05
#define CTRL_K 0x0B
#define CTRL_L 0x0C
#define CTRL_U 0x15
#define CTRL_W 0x17
#define BS     0x08
#define DEL    0x7F
#define ESC    0x1B
#define LF     0x0A
#define CR     0x0D

static char s_buf[LINEEDIT_CAP];
static size_t s_len;   // байт до нуля
static size_t s_cur;   // байтовое смещение курсора, 0..s_len

// Разбор escape-последовательности. Приходит по байту: ESC, затем '[', затем
// финальный символ. Состояние нужно потому, что между этими байтами цикл
// успевает вернуться к чтению порта.
typedef enum {
    ESC_NONE = 0,
    ESC_START,     // получили ESC
    ESC_CSI,       // получили ESC [
    ESC_CSI_NUM,   // получили ESC [ и цифру: ждём '~' (Delete, Home, End)
} esc_state;

static esc_state s_esc;
static int s_esc_num;

// Номер записи истории, которую сейчас показывает редактор, или -1, если
// показывается то, что набрано. Черновик сохраняется отдельно: нажав ↑ и
// передумав, надо вернуть ровно то, что было набрано, а не пустую строку.
static int s_hist;
static char s_draft[LINEEDIT_CAP];

static le_event bell(void)
{
    return LE_BELL;
}

static le_event changed(void)
{
    // Любая правка отвязывает строку от истории: иначе Enter записал бы
    // изменённую копию старой команды как новую, а ↓ вернул бы к черновику,
    // которого уже нет.
    s_hist = -1;
    return LE_CHANGED;
}

void le_init(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    memset(s_draft, 0, sizeof(s_draft));
    s_len = 0;
    s_cur = 0;
    s_esc = ESC_NONE;
    s_esc_num = 0;
    s_hist = -1;
    history_init();
}

void le_reset(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    s_len = 0;
    s_cur = 0;
    s_esc = ESC_NONE;
    s_esc_num = 0;
    s_hist = -1;
}

void le_set_line(const char* text)
{
    if (text == NULL) {
        le_reset();
        return;
    }

    size_t n = strlen(text);
    if (n >= LINEEDIT_CAP) {
        n = LINEEDIT_CAP - 1;
    }
    memcpy(s_buf, text, n);
    s_buf[n] = '\0';
    s_len = n;
    s_cur = n;
    s_hist = -1;
}

const char* le_line(void)
{
    return s_buf;
}

size_t le_len(void)
{
    return s_len;
}

size_t le_cursor(void)
{
    return s_cur;
}

// Число клеток в куске буфера: клетка - это символ, то есть байт, который не
// является продолжением UTF-8. Так считать дешевле, чем разбирать заново:
// продолжение всегда имеет старшие биты 10.
static size_t count_cells(const char* s, size_t bytes)
{
    size_t cells = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (((uint8_t)s[i] & 0xC0u) != 0x80u) {
            ++cells;
        }
    }
    return cells;
}

size_t le_cols(void)
{
    return count_cells(s_buf, s_len);
}

size_t le_cursor_col(void)
{
    return count_cells(s_buf, s_cur);
}

// Собрать символ, начинающийся в байте at. Возвращает код, а в *next -
// смещение следующего символа. Декодер общий с tty: своя арифметика сдвигов
// разошлась бы с ним на первом же трёхбайтном символе.
static uint32_t cp_at(size_t at, size_t* next)
{
    utf8_decoder d;
    utf8_reset(&d);

    uint32_t cp = 0;
    utf8_status st = UTF8_PENDING;
    while (s_buf[at] != '\0') {
        st = utf8_feed(&d, (uint8_t)s_buf[at], &cp);
        ++at;
        if (st != UTF8_PENDING) {
            break;
        }
    }

    *next = at;
    // Битая последовательность в буфер попасть не может (вставляет его только
    // insert_cp, а он проверяет код), но le_set_line принимает любые байты.
    // Рисуется тогда «?», а не пустая клетка.
    return st == UTF8_DONE ? cp : 0xFFFDu;
}

uint32_t le_cell_cp(size_t index)
{
    size_t at = 0;
    size_t cell = 0;

    while (at < s_len) {
        // Байт-продолжение - это не клетка: иначе «Ж» заняла бы две.
        if (((uint8_t)s_buf[at] & 0xC0u) == 0x80u) {
            ++at;
            continue;
        }

        size_t next = at;
        const uint32_t cp = cp_at(at, &next);
        if (cell == index) {
            return cp;
        }
        ++cell;
        at = next;
    }

    return 0;
}

// --- движение курсора ---

// Начало предыдущего символа. Идём назад через байты-продолжения: у «Ж» их
// один, и курсор должен встать на головку, а не в середину буквы.
static size_t prev_char(size_t at)
{
    if (at == 0) {
        return 0;
    }
    --at;
    while (at > 0 && ((uint8_t)s_buf[at] & 0xC0u) == 0x80u) {
        --at;
    }
    return at;
}

// Начало следующего символа.
static size_t next_char(size_t at)
{
    if (at >= s_len) {
        return s_len;
    }
    ++at;
    while (at < s_len && ((uint8_t)s_buf[at] & 0xC0u) == 0x80u) {
        ++at;
    }
    return at;
}

static le_event move_left(void)
{
    if (s_cur == 0) {
        return bell();
    }
    s_cur = prev_char(s_cur);
    return LE_CHANGED;
}

static le_event move_right(void)
{
    if (s_cur >= s_len) {
        return bell();
    }
    s_cur = next_char(s_cur);
    return LE_CHANGED;
}

static le_event move_home(void)
{
    if (s_cur == 0) {
        return LE_NONE;
    }
    s_cur = 0;
    return LE_CHANGED;
}

static le_event move_end(void)
{
    if (s_cur == s_len) {
        return LE_NONE;
    }
    s_cur = s_len;
    return LE_CHANGED;
}

// --- правка ---

// Вырезать байты [from, to). Один memmove, а не посимвольно: иначе на длинной
// строке каждое стирание было бы квадратичным, а плата это заметит.
static le_event cut(size_t from, size_t to)
{
    if (from >= to) {
        return bell();
    }

    memmove(s_buf + from, s_buf + to, s_len - to + 1);
    s_len -= to - from;
    s_cur = from;
    return changed();
}

static le_event insert_cp(uint32_t cp)
{
    char enc[4];
    const size_t n = utf8_encode(cp, enc);
    if (n == 0) {
        // Не символ (суррогат, за пределами Unicode) - рисовать нечего.
        return LE_NONE;
    }
    if (s_len + n >= LINEEDIT_CAP) {
        return bell();
    }

    memmove(s_buf + s_cur + n, s_buf + s_cur, s_len - s_cur + 1);
    memcpy(s_buf + s_cur, enc, n);
    s_len += n;
    s_cur += n;
    return changed();
}

static le_event backspace(void)
{
    if (s_cur == 0) {
        return bell();
    }
    return cut(prev_char(s_cur), s_cur);
}

static le_event delete_forward(void)
{
    if (s_cur >= s_len) {
        return bell();
    }
    return cut(s_cur, next_char(s_cur));
}

static le_event kill_to_start(void)
{
    if (s_cur == 0) {
        return bell();
    }
    return cut(0, s_cur);
}

static le_event kill_to_end(void)
{
    if (s_cur >= s_len) {
        return bell();
    }
    return cut(s_cur, s_len);
}

// Ctrl-W: стереть слово перед курсором. Сначала пропускаем пробелы - иначе
// после «cmd arg1 » нажатие съело бы только пробел, и слово пришлось бы
// стирать вторым нажатием.
static le_event kill_word(void)
{
    size_t from = s_cur;
    while (from > 0 && s_buf[prev_char(from)] == ' ') {
        from = prev_char(from);
    }
    while (from > 0 && s_buf[prev_char(from)] != ' ') {
        from = prev_char(from);
    }
    return cut(from, s_cur);
}

// --- история ---

static void load(const char* text)
{
    size_t n = strlen(text);
    if (n >= LINEEDIT_CAP) {
        n = LINEEDIT_CAP - 1;
    }
    memcpy(s_buf, text, n);
    s_buf[n] = '\0';
    s_len = n;
    s_cur = n;
}

static le_event history_prev(void)
{
    const int count = history_count();
    if (count == 0) {
        return bell();
    }

    if (s_hist == -1) {
        // Уходим из черновика: запоминаем набранное, чтобы вернуть его на ↓.
        memcpy(s_draft, s_buf, sizeof(s_draft));
        s_hist = count - 1;
    } else if (s_hist > 0) {
        --s_hist;
    } else {
        return bell();
    }

    load(history_get(s_hist));
    return LE_CHANGED;
}

static le_event history_next(void)
{
    if (s_hist == -1) {
        return bell();
    }

    if (s_hist < history_count() - 1) {
        ++s_hist;
        load(history_get(s_hist));
        return LE_CHANGED;
    }

    // Дошли до новой строки: возвращаем черновик, который был до листания.
    s_hist = -1;
    load(s_draft);
    return LE_CHANGED;
}

// --- escape-последовательности ---

// Стрелка, Home, End и Delete приходят тремя-четырьмя байтами. Набор разный
// у терминалов, и поддерживать все варианты незачем: xterm, Terminal и iTerm
// шлют именно эти.
static le_event escape_final(uint32_t cp)
{
    switch (cp) {
    case 'A':
        return history_prev();
    case 'B':
        return history_next();
    case 'C':
        return move_right();
    case 'D':
        return move_left();
    case 'H':
        return move_home();
    case 'F':
        return move_end();
    default:
        return LE_NONE;
    }
}

static le_event escape_digit(uint32_t cp)
{
    if (cp == '~') {
        switch (s_esc_num) {
        case 1:
        case 7:
            return move_home();
        case 4:
        case 8:
            return move_end();
        case 3:
            return delete_forward();
        default:
            return LE_NONE;
        }
    }

    if (cp >= '0' && cp <= '9') {
        s_esc_num = s_esc_num * 10 + (int)(cp - '0');
        if (s_esc_num > 99) {
            s_esc = ESC_NONE;
        }
        return LE_NONE;
    }

    s_esc = ESC_NONE;
    return LE_NONE;
}

static le_event escape_input(uint32_t cp)
{
    switch (s_esc) {
    case ESC_START:
        if (cp == '[') {
            s_esc = ESC_CSI;
            s_esc_num = 0;
            return LE_NONE;
        }
        // Alt-клавиша или одиночный ESC: не поддерживаем, но и не теряем
        // состояние - следующий байт должен разбираться как обычный.
        s_esc = ESC_NONE;
        return LE_NONE;

    case ESC_CSI:
        if (cp >= '0' && cp <= '9') {
            s_esc_num = (int)(cp - '0');
            s_esc = ESC_CSI_NUM;
            return LE_NONE;
        }
        s_esc = ESC_NONE;
        // Финальный символ разбираем сразу: состояние уже сброшено, но
        // возвращать NONE значило бы потерять клавишу.
        return escape_final(cp);

    case ESC_CSI_NUM:
        return escape_digit(cp);

    case ESC_NONE:
    default:
        return LE_NONE;
    }
}

// --- вход ---

le_event le_input(uint32_t cp)
{
    if (s_esc != ESC_NONE || cp == ESC) {
        if (cp == ESC && s_esc == ESC_NONE) {
            s_esc = ESC_START;
            return LE_NONE;
        }
        return escape_input(cp);
    }

    switch (cp) {
    case CR:
    case LF:
        // Запись в историю здесь, а не у вызывающего: редактор один знает,
        // что строка закончена и что набранное больше не принадлежит истории.
        history_push(s_buf);
        s_hist = -1;
        return LE_SUBMIT;

    case CTRL_C:
        s_hist = -1;
        return LE_INTR;

    case CTRL_D:
        // Устоявшееся поведение: на пустой строке - конец ввода, иначе
        // удаление символа под курсором (как Delete, но без стрелки).
        if (s_len == 0) {
            s_hist = -1;
            return LE_EOF;
        }
        return delete_forward();

    case CTRL_A:
        return move_home();

    case CTRL_E:
        return move_end();

    case CTRL_K:
        return kill_to_end();

    case CTRL_U:
        return kill_to_start();

    case CTRL_W:
        return kill_word();

    case BS:
    case DEL:
        return backspace();

    case CTRL_L:
        // Перерисовать экран. Редактор только сообщает, что показать надо
        // заново; что именно значит «заново», решает вызывающий.
        return LE_REDRAW;

    default:
        break;
    }

    if (cp < 0x20) {
        // Прочие управляющие (Tab, Ctrl-Z, Ctrl-S/Q) ничего не делают. Делать
        // вид, что нажатие сработало, нельзя: это учит неверной привычке.
        return LE_NONE;
    }

    return insert_cp(cp);
}
