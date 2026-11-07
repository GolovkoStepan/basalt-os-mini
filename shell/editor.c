// Ядро полноэкранного редактора. См. editor.h - там про то, зачем отдельно.

#include "editor.h"

#include <string.h>

// Разбор escape-последовательности. Тот же автомат, что в lineedit: ESC,
// ESC [, ESC [ <число> ~. Других вариантов терминалы, с которыми мы работаем,
// не шлют, а поддержка всех - это отдельный слой, которого пока не за что
// зацепить.
enum {
    ES_NONE = 0,
    ES_START,  // получили ESC
    ES_CSI,    // получили ESC [
    ES_NUM,    // получили ESC [ и цифру: ждём '~'
};

// ---------------------------------------------------------------------------
// Проход по тексту
//
// Текст - один буфер UTF-8 с '\n'. Строка ищется проходом от начала, а не
// индексом: индекс пришлось бы править на каждой вставке, и расхождение его с
// текстом было бы самой дорогой ошибкой в редакторе. Файл на 64 КБ проходится
// за микросекунды, а нажатий человек делает единицы в секунду.
// ---------------------------------------------------------------------------

// Символ в позиции pos: code point и сколько байт он занял. Недопустимая
// последовательность читается как один байт и код '?': иначе курсор встал бы
// внутрь битой буквы и стирание резало бы её по половине.
static uint32_t cp_at(const char* s, uint32_t len, uint32_t pos, uint32_t* cp)
{
    utf8_decoder d;
    utf8_reset(&d);

    for (uint32_t i = 0; i < 4 && pos + i < len; ++i) {
        uint32_t v = 0;
        const utf8_status st = utf8_feed(&d, (uint8_t)s[pos + i], &v);
        if (st == UTF8_DONE) {
            *cp = v;
            return i + 1;
        }
        if (st == UTF8_INVALID) {
            break;
        }
    }

    *cp = '?';
    return 1;
}

// Начало строки, содержащей pos. pos за концом читается как конец: так
// вызывающему не нужно проверять границу перед каждым вызовом.
static uint32_t line_start_of(const editor* ed, uint32_t pos)
{
    if (pos > ed->len) {
        pos = ed->len;
    }
    while (pos > 0 && ed->buf[pos - 1] != '\n') {
        --pos;
    }
    return pos;
}

// Смещение '\n' в конце строки или len, если перевод строки не найден.
static uint32_t line_end_of(const editor* ed, uint32_t start)
{
    uint32_t p = start;
    while (p < ed->len && ed->buf[p] != '\n') {
        ++p;
    }
    return p;
}

// Номер строки (с нуля), в которой лежит pos.
static uint32_t line_index(const editor* ed, uint32_t pos)
{
    if (pos > ed->len) {
        pos = ed->len;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < pos; ++i) {
        if (ed->buf[i] == '\n') {
            ++n;
        }
    }
    return n;
}

// Смещение начала строки с номером index. За последней строкой - len.
static uint32_t line_offset(const editor* ed, uint32_t index)
{
    uint32_t p = 0;
    while (index > 0 && p < ed->len) {
        if (ed->buf[p] == '\n') {
            --index;
        }
        ++p;
    }
    return p;
}

// Начало предыдущей строки. false - уже первая.
static bool prev_line_start(const editor* ed, uint32_t start, uint32_t* out)
{
    if (start == 0) {
        return false;
    }
    *out = line_start_of(ed, start - 1);
    return true;
}

// Ширина символа в клетках от колонки col. Табуляция - до следующей кратной
// восьми, как в матрице: иначе один и тот же файл в редакторе и в `less`
// съезжал бы по-разному.
static uint32_t cell_width(uint32_t cp, uint32_t col)
{
    if (cp == '\t') {
        return EDITOR_TAB - (col % EDITOR_TAB);
    }
    return 1;
}

// Сколько клеток занимает текст от from до to.
static uint32_t cells_to(const editor* ed, uint32_t from, uint32_t to)
{
    uint32_t col = 0;
    uint32_t p = from;
    while (p < to && p < ed->len) {
        uint32_t cp = 0;
        p += cp_at(ed->buf, ed->len, p, &cp);
        col += cell_width(cp, col);
    }
    return col;
}

// Встать в строку start на колонку goal (в клетках). Если строка короче -
// в её конец: стрелка вниз не должна упираться, как в стену.
static uint32_t place_in_line(const editor* ed, uint32_t start, uint32_t goal)
{
    const uint32_t end = line_end_of(ed, start);
    uint32_t p = start;
    uint32_t col = 0;

    while (p < end) {
        uint32_t cp = 0;
        const uint32_t used = cp_at(ed->buf, ed->len, p, &cp);
        const uint32_t next = col + cell_width(cp, col);
        if (next > goal) {
            // Цель попала внутрь табуляции - встаём перед ней, а не после:
            // иначе курсор прыгнул бы на восемь клеток вперёд.
            break;
        }
        col = next;
        p += used;
    }
    return p;
}

// Начало предыдущего символа. Байт-продолжение (10xxxxxx) - не начало.
static uint32_t prev_cp_start(const editor* ed, uint32_t pos)
{
    if (pos == 0) {
        return 0;
    }
    uint32_t p = pos - 1;
    while (p > 0 && ((uint8_t)ed->buf[p] & 0xC0u) == 0x80u) {
        --p;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Окно
// ---------------------------------------------------------------------------

// Подтянуть окно к курсору: курсор не должен уезжать за экран ни по
// вертикали, ни по горизонтали. Вызывается после каждого изменения позиции -
// одна точка вместо проверок в каждом обработчике клавиши.
static void follow(editor* ed)
{
    // top обязан быть началом строки. После удаления выше окна он мог
    // оказаться в середине чужой строки, и окно показало бы обрезанный текст.
    ed->top = line_offset(ed, line_index(ed, ed->top));

    const uint32_t cs = line_start_of(ed, ed->cursor);
    const uint32_t col = cells_to(ed, cs, ed->cursor);

    if (col < ed->left) {
        ed->left = col;
    }
    if (col >= ed->left + EDITOR_COLS) {
        ed->left = col - EDITOR_COLS + 1;
    }

    const uint32_t cl = line_index(ed, cs);
    const uint32_t tl = line_index(ed, ed->top);
    if (cl < tl) {
        ed->top = cs;
    } else if (cl >= tl + EDITOR_ROWS) {
        // Курсор должен оказаться в последней строке окна.
        uint32_t t = cs;
        for (uint32_t i = 0; i + 1 < EDITOR_ROWS; ++i) {
            uint32_t prev = 0;
            if (!prev_line_start(ed, t, &prev)) {
                break;
            }
            t = prev;
        }
        ed->top = t;
    }
}

// Курсор сдвинулся или текст изменился: пересчитать желаемую колонку и окно.
static void sync_view(editor* ed)
{
    ed->goal = cells_to(ed, line_start_of(ed, ed->cursor), ed->cursor);
    follow(ed);
}

// ---------------------------------------------------------------------------
// Правка
// ---------------------------------------------------------------------------

static bool insert_bytes(editor* ed, const char* src, uint32_t n)
{
    if (n == 0 || n > ed->cap - ed->len) {
        return false;
    }
    memmove(ed->buf + ed->cursor + n, ed->buf + ed->cursor, ed->len - ed->cursor);
    memcpy(ed->buf + ed->cursor, src, n);
    ed->len += n;
    ed->cursor += n;
    ed->dirty = true;
    return true;
}

static bool erase_range(editor* ed, uint32_t from, uint32_t to)
{
    if (from >= to) {
        return false;
    }
    memmove(ed->buf + from, ed->buf + to, ed->len - to);
    ed->len -= (to - from);
    if (ed->cursor >= to) {
        ed->cursor -= (to - from);
    } else if (ed->cursor > from) {
        ed->cursor = from;
    }
    ed->dirty = true;
    return true;
}

static ed_event insert_cp(editor* ed, uint32_t cp)
{
    char enc[4];
    const size_t n = utf8_encode(cp, enc);
    if (n == 0 || !insert_bytes(ed, enc, (uint32_t)n)) {
        return ED_BELL;
    }
    sync_view(ed);
    return ED_CHANGED;
}

static ed_event insert_char(editor* ed, char c)
{
    if (!insert_bytes(ed, &c, 1)) {
        return ED_BELL;
    }
    sync_view(ed);
    return ED_CHANGED;
}

static ed_event backspace(editor* ed)
{
    if (ed->cursor == 0) {
        return ED_BELL;
    }
    const uint32_t p = prev_cp_start(ed, ed->cursor);
    erase_range(ed, p, ed->cursor);
    ed->cursor = p;
    sync_view(ed);
    return ED_CHANGED;
}

static ed_event delete_forward(editor* ed)
{
    if (ed->cursor >= ed->len) {
        return ED_BELL;
    }
    uint32_t cp = 0;
    const uint32_t n = cp_at(ed->buf, ed->len, ed->cursor, &cp);
    erase_range(ed, ed->cursor, ed->cursor + n);
    sync_view(ed);
    return ED_CHANGED;
}

static ed_event move_home(editor* ed)
{
    ed->cursor = line_start_of(ed, ed->cursor);
    sync_view(ed);
    return ED_CHANGED;
}

static ed_event move_end(editor* ed)
{
    ed->cursor = line_end_of(ed, line_start_of(ed, ed->cursor));
    sync_view(ed);
    return ED_CHANGED;
}

static ed_event move_left(editor* ed)
{
    if (ed->cursor == 0) {
        return ED_NONE;
    }
    const uint32_t ls = line_start_of(ed, ed->cursor);
    if (ed->cursor == ls) {
        // Начало строки: уходим в конец предыдущей - так ведёт себя любой
        // редактор, и иначе стрелка влево «застревала» бы на каждой строке.
        ed->cursor = ls - 1;
    } else {
        ed->cursor = prev_cp_start(ed, ed->cursor);
    }
    sync_view(ed);
    return ED_CHANGED;
}

static ed_event move_right(editor* ed)
{
    if (ed->cursor >= ed->len) {
        return ED_NONE;
    }
    uint32_t cp = 0;
    ed->cursor += cp_at(ed->buf, ed->len, ed->cursor, &cp);
    sync_view(ed);
    return ED_CHANGED;
}

// Стрелки вверх и вниз ходят по строкам и держат желаемую колонку: без неё
// проход через короткую строку сдвигал бы курсор влево, и вернуться на
// прежнее место было бы нечем.
static ed_event move_up(editor* ed)
{
    const uint32_t ls = line_start_of(ed, ed->cursor);
    uint32_t prev = 0;
    if (!prev_line_start(ed, ls, &prev)) {
        return ED_NONE;
    }
    ed->cursor = place_in_line(ed, prev, ed->goal);
    follow(ed);
    return ED_CHANGED;
}

static ed_event move_down(editor* ed)
{
    const uint32_t end = line_end_of(ed, line_start_of(ed, ed->cursor));
    if (end >= ed->len) {
        return ED_NONE;
    }
    // Перевод строки в самом конце файла - не пустая строка: «a\n» это одна
    // строка, и вниз с неё вести некуда. Иначе редактор показывал бы на одну
    // строку больше, чем `less`, и номер строки в углу не совпадал бы с тем,
    // что видно при просмотре.
    if (end + 1 >= ed->len) {
        return ED_NONE;
    }
    ed->cursor = place_in_line(ed, end + 1, ed->goal);
    follow(ed);
    return ED_CHANGED;
}

static ed_event page_up(editor* ed)
{
    for (int i = 0; i < EDITOR_ROWS; ++i) {
        move_up(ed);
    }
    return ED_CHANGED;
}

static ed_event page_down(editor* ed)
{
    for (int i = 0; i < EDITOR_ROWS; ++i) {
        move_down(ed);
    }
    return ED_CHANGED;
}

// ---------------------------------------------------------------------------
// Клавиши
// ---------------------------------------------------------------------------

static ed_event esc_final(editor* ed, uint32_t cp)
{
    switch (cp) {
    case 'A':
        return move_up(ed);
    case 'B':
        return move_down(ed);
    case 'C':
        return move_right(ed);
    case 'D':
        return move_left(ed);
    case 'H':
        return move_home(ed);
    case 'F':
        return move_end(ed);
    default:
        return ED_NONE;
    }
}

static ed_event esc_digit(editor* ed, uint32_t cp)
{
    if (cp == '~') {
        switch (ed->esc_num) {
        case 1:
        case 7:
            return move_home(ed);
        case 4:
        case 8:
            return move_end(ed);
        case 3:
            return delete_forward(ed);
        case 5:
            return page_up(ed);
        case 6:
            return page_down(ed);
        default:
            return ED_NONE;
        }
    }

    if (cp >= '0' && cp <= '9') {
        ed->esc_num = ed->esc_num * 10 + (int)(cp - '0');
        if (ed->esc_num > 999) {
            ed->esc = ES_NONE;
        }
        return ED_NONE;
    }

    ed->esc = ES_NONE;
    return ED_NONE;
}

static ed_event esc_input(editor* ed, uint32_t cp)
{
    switch (ed->esc) {
    case ES_START:
        if (cp == '[') {
            ed->esc = ES_CSI;
            ed->esc_num = 0;
            return ED_NONE;
        }
        // Alt-клавиша или одиночный ESC: не поддерживаем, но и не виснем.
        ed->esc = ES_NONE;
        return ED_NONE;

    case ES_CSI:
        if (cp >= '0' && cp <= '9') {
            ed->esc_num = (int)(cp - '0');
            ed->esc = ES_NUM;
            return ED_NONE;
        }
        ed->esc = ES_NONE;
        return esc_final(ed, cp);

    case ES_NUM:
        return esc_digit(ed, cp);

    default:
        ed->esc = ES_NONE;
        return ED_NONE;
    }
}

static ed_event key(editor* ed, uint32_t cp)
{
    // Подтверждение выхода снимается любой другой клавишей: «Ctrl-Q ещё раз»
    // относится к следующему нажатию, а не к любому через минуту.
    if (cp != 0x11u && cp != 0x03u) {
        ed->quit_armed = false;
    }

    switch (cp) {
    case 0x11u:  // Ctrl-Q
    case 0x03u:  // Ctrl-C
        if (ed->dirty && !ed->quit_armed) {
            ed->quit_armed = true;
            return ED_QUIT_DIRTY;
        }
        return ED_QUIT;

    case 0x13u:  // Ctrl-S
        return ED_SAVE;

    case 0x06u:  // Ctrl-F
        return ED_FIND;

    case '\r':
    case '\n':
        return insert_char(ed, '\n');

    case '\t':
        return insert_char(ed, '\t');

    case 0x7Fu:
    case 0x08u:
        return backspace(ed);

    default:
        break;
    }

    // Остальные управляющие коды - не текст: они либо команды (разобраны
    // выше), либо мусор от терминала, и записывать их в файл незачем.
    if (cp < 0x20u) {
        return ED_BELL;
    }
    return insert_cp(ed, cp);
}

// ---------------------------------------------------------------------------
// Публичный интерфейс
// ---------------------------------------------------------------------------

void ed_init(editor* ed, char* buf, uint32_t cap)
{
    memset(ed, 0, sizeof(*ed));
    ed->buf = buf;
    ed->cap = cap;
    utf8_reset(&ed->dec);
}

void ed_load(editor* ed, uint32_t len)
{
    if (len > ed->cap) {
        len = ed->cap;
    }

    uint32_t src = 0;
    if (len >= 3 && (uint8_t)ed->buf[0] == 0xEF && (uint8_t)ed->buf[1] == 0xBB &&
        (uint8_t)ed->buf[2] == 0xBF) {
        src = 3;
    }

    // Запись идёт на месте и только влево (dst <= src), поэтому второй буфер
    // на 64 КБ не нужен.
    uint32_t dst = 0;
    while (src < len) {
        const char c = ed->buf[src++];
        if (c == '\r' && src < len && ed->buf[src] == '\n') {
            continue;  // CRLF -> LF: CR выбрасываем, LF скопируется следующим
        }
        ed->buf[dst++] = c;
    }

    ed->len = dst;
    ed->cursor = 0;
    ed->top = 0;
    ed->left = 0;
    ed->goal = 0;
    ed->dirty = false;
    ed->quit_armed = false;
    ed->esc = ES_NONE;
    ed->esc_num = 0;
    utf8_reset(&ed->dec);
}

const char* ed_data(const editor* ed)
{
    return ed->buf;
}

uint32_t ed_len(const editor* ed)
{
    return ed->len;
}

bool ed_dirty(const editor* ed)
{
    return ed->dirty;
}

void ed_mark_saved(editor* ed)
{
    ed->dirty = false;
    ed->quit_armed = false;
}

ed_event ed_input(editor* ed, uint8_t byte)
{
    if (ed->esc != ES_NONE || byte == 0x1B) {
        // Недособранная буква уже не продолжится: ESC начинает команду.
        utf8_reset(&ed->dec);
        ed->quit_armed = false;
        if (byte == 0x1B && ed->esc == ES_NONE) {
            ed->esc = ES_START;
            return ED_NONE;
        }
        return esc_input(ed, byte);
    }

    uint32_t cp = 0;
    const utf8_status st = utf8_feed(&ed->dec, byte, &cp);
    if (st == UTF8_PENDING) {
        return ED_NONE;
    }
    if (st == UTF8_INVALID) {
        return ED_BELL;
    }
    return key(ed, cp);
}

int ed_search(editor* ed, const char* needle)
{
    if (needle == NULL || needle[0] == '\0') {
        return -1;
    }

    const uint32_t n = (uint32_t)strlen(needle);
    if (n > ed->len) {
        return -1;
    }

    // Ищем со следующего символа: иначе второе Ctrl-F находит то же самое, и
    // поиск выглядит сломанным. До конца файла, потом с начала - «дальше»
    // без каретки поиска не сделать, а спрашивать два режима незачем.
    const uint32_t from = (ed->cursor < ed->len) ? ed->cursor + 1 : ed->len;
    uint32_t hit = ed->len + 1;
    for (uint32_t i = from; i + n <= ed->len; ++i) {
        if (memcmp(ed->buf + i, needle, n) == 0) {
            hit = i;
            break;
        }
    }
    if (hit > ed->len) {
        for (uint32_t i = 0; i + n <= ed->len; ++i) {
            if (memcmp(ed->buf + i, needle, n) == 0) {
                hit = i;
                break;
            }
        }
    }
    if (hit > ed->len) {
        return -1;
    }

    ed->cursor = hit;
    sync_view(ed);
    return 0;
}

int ed_row(const editor* ed, int row, uint16_t* cells, int cap)
{
    if (row < 0 || row >= EDITOR_ROWS || cap <= 0) {
        return 0;
    }

    uint32_t start = ed->top;
    for (int i = 0; i < row; ++i) {
        if (start >= ed->len) {
            return 0;
        }
        const uint32_t end = line_end_of(ed, start);
        if (end >= ed->len) {
            // Последняя строка без перевода: ниже неё строк нет.
            return 0;
        }
        start = end + 1;
    }
    if (start > ed->len) {
        return 0;
    }

    uint32_t p = start;
    uint32_t col = 0;
    int n = 0;

    while (p < ed->len && n < cap) {
        uint32_t cp = 0;
        const uint32_t used = cp_at(ed->buf, ed->len, p, &cp);
        if (cp == '\n') {
            break;
        }
        p += used;

        if (cp == '\t') {
            const uint32_t next = col + cell_width(cp, col);
            while (col < next) {
                if (col >= ed->left) {
                    cells[n++] = ' ';
                }
                ++col;
            }
            continue;
        }

        // Управляющие байты и символы вне BMP глифа не имеют: показываем '?',
        // иначе строка молча съехала бы влево на один символ.
        if (cp < 0x20u || cp == 0x7Fu || cp > 0xFFFFu) {
            cp = '?';
        }

        if (col >= ed->left) {
            cells[n++] = (uint16_t)cp;
        }
        ++col;
    }
    return n;
}

void ed_cursor_screen(const editor* ed, int* row, int* col)
{
    const uint32_t cs = line_start_of(ed, ed->cursor);
    const uint32_t cl = line_index(ed, cs);
    const uint32_t tl = line_index(ed, ed->top);

    int r = (int)(cl - tl);
    int c = (int)(cells_to(ed, cs, ed->cursor) - ed->left);

    if (r < 0) {
        r = 0;
    }
    if (r >= EDITOR_ROWS) {
        r = EDITOR_ROWS - 1;
    }
    if (c < 0) {
        c = 0;
    }
    if (c >= EDITOR_COLS) {
        c = EDITOR_COLS - 1;
    }
    *row = r;
    *col = c;
}

void ed_line_col(const editor* ed, uint32_t* line, uint32_t* col)
{
    const uint32_t cs = line_start_of(ed, ed->cursor);
    if (line != NULL) {
        *line = line_index(ed, cs) + 1;
    }
    if (col != NULL) {
        *col = cells_to(ed, cs, ed->cursor) + 1;
    }
}
