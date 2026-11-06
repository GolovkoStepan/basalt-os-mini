// Line discipline. Зачем отдельный слой - см. tty.h.

#include "tty.h"

#include <string.h>

#define CTRL_C 0x03
#define CTRL_D 0x04
#define BS     0x08
#define DEL    0x7F
#define LF     0x0A
#define CR     0x0D

// Редактируемая строка. NUL-терминатор держим всегда: так вызывающему не
// нужно помнить длину, а печать в лог и в матрицу становится одной строкой
// кода. Место под него отнимает один байт вместимости - это дешевле, чем
// отдельный счётчик в каждом вызывающем.
static char s_line[TTY_LINE_CAP];
static size_t s_len;

// Где началась текущая многобайтовая последовательность. Нужно, чтобы
// откатить буфер, если она окажется битой: utf8_feed узнаёт об этом только
// на последнем байте, а байты к тому времени уже в строке.
static size_t s_seq_start;

static tty_mode s_mode;
static utf8_decoder s_utf8;

// Строка отдана, но ещё не затёрта: tty_line() должна быть годна, пока
// вызывающий её разбирает. Сбрасываем при следующем байте, а не сразу, -
// иначе обработать строку было бы нечем.
static bool s_ready;

// Предыдущий байт был CR. Нужен, чтобы съесть LF из пары CR+LF.
static bool s_cr;

void tty_init(void)
{
    memset(s_line, 0, sizeof(s_line));
    s_len = 0;
    s_seq_start = 0;
    s_mode = TTY_MODE_CANON;
    s_ready = false;
    s_cr = false;
    utf8_reset(&s_utf8);
}

void tty_set_mode(tty_mode mode)
{
    if (mode == s_mode) {
        return;
    }
    // Буфер выбрасываем: недособранная строка принадлежала прошлому режиму,
    // и «дописать» её в новый значило бы показать программе в raw половину
    // команды, которую набирали для канона.
    s_mode = mode;
    s_len = 0;
    s_seq_start = 0;
    s_line[0] = '\0';
    s_ready = false;
    s_cr = false;
    utf8_reset(&s_utf8);
}

tty_mode tty_mode_get(void)
{
    return s_mode;
}

const char* tty_line(void)
{
    return s_line;
}

size_t tty_line_len(void)
{
    return s_len;
}

const char* tty_pending(void)
{
    return s_line;
}

size_t tty_pending_len(void)
{
    return s_len;
}


static tty_result make(tty_event event, uint32_t cp)
{
    tty_result r;
    r.event = event;
    r.cp = cp;
    return r;
}

// Откатить строку на один символ. UTF-8 хранится байтами, и Backspace обязан
// стереть всю букву, а не последний байт: иначе от «Ж» осталась бы половина
// последовательности, и следующая буква склеилась бы с ней.
static void trim_last_cp(void)
{
    if (s_len == 0) {
        return;
    }

    --s_len;
    // Байты-продолжения имеют старшие биты 10; пока они идут - буква началась
    // раньше. У ASCII старший бит нулевой, поэтому цикл на нём и остановится.
    while (s_len > 0 && ((uint8_t)s_line[s_len] & 0xC0u) == 0x80u) {
        --s_len;
    }
    s_line[s_len] = '\0';
}

// Записать байт в строку. Возвращает false, если места нет: тогда вызывающий
// сообщает BELL, а не молча теряет нажатие.
static bool push_byte(uint8_t byte)
{
    if (s_len + 1 >= TTY_LINE_CAP) {
        s_line[s_len] = '\0';
        return false;
    }
    s_line[s_len++] = (char)byte;
    s_line[s_len] = '\0';
    return true;
}

// Общая часть для обоих режимов: собрать символ из байтов UTF-8 и положить
// его в буфер. В raw буфер не нужен, поэтому там эта функция не зовётся -
// см. tty_input.
static tty_result input_multibyte(uint8_t byte)
{
    if (s_utf8.need == 0) {
        // Начало новой последовательности: запомним, откуда откатывать.
        s_seq_start = s_len;
    }

    if (!push_byte(byte)) {
        // Место кончилось посреди буквы. Откатываем огрызок, иначе в строке
        // остались бы байты без продолжения.
        s_len = s_seq_start;
        s_line[s_len] = '\0';
        utf8_reset(&s_utf8);
        return make(TTY_EV_BELL, 0);
    }

    uint32_t cp = 0;
    const utf8_status st = utf8_feed(&s_utf8, byte, &cp);

    if (st == UTF8_PENDING) {
        return make(TTY_EV_NONE, 0);
    }
    if (st == UTF8_INVALID) {
        // Битая последовательность: байты уже в буфере, убираем их. Курсор на
        // экране при этом не двигался - рисовать было нечего.
        s_len = s_seq_start;
        s_line[s_len] = '\0';
        return make(TTY_EV_NONE, 0);
    }

    return make(TTY_EV_CHAR, cp);
}

// Откатить недособранную последовательность. utf8_feed узнаёт, что она битая,
// только когда приходит не тот байт; к этому моменту байты уже в строке, и без
// отката они остались бы там навсегда - в команде появился бы невидимый мусор,
// а длина строки не совпала бы с набранным.
static void drop_partial(void)
{
    if (s_utf8.need == 0) {
        return;
    }
    s_len = s_seq_start;
    s_line[s_len] = '\0';
    utf8_reset(&s_utf8);
}

tty_result tty_input(uint8_t byte)
{
    // Enter приходит по-разному: терминал мака присылает CR, а `pio device
    // monitor` с EOL=CRLF - пару CR+LF. Для нас это одно нажатие: иначе строка
    // отправлялась бы дважды, и после каждой команды появлялась бы лишняя
    // пустая строка с приглашением (проверено на живой плате 06.11.2026).
    // Поэтому LF сразу за CR не отдаём никому: ни raw, ни канону.
    if (byte == LF && s_cr) {
        s_cr = false;
        return make(TTY_EV_NONE, 0);
    }
    s_cr = (byte == CR);

    // Предыдущая строка уже отдана и прочитана (вызывающий успел её
    // разобрать, потому что обрабатывает событие до следующего чтения порта).
    // Теперь начинается новая.
    if (s_ready) {
        s_ready = false;
        s_len = 0;
        s_seq_start = 0;
        s_line[0] = '\0';
    }

    // Ctrl-C работает в обоих режимах: из raw иначе не выбраться, а выйти
    // должно быть можно всегда, даже если программа зависла.
    if (byte == CTRL_C) {
        s_len = 0;
        s_seq_start = 0;
        s_line[0] = '\0';
        utf8_reset(&s_utf8);
        return make(TTY_EV_INTR, 0);
    }

    if (s_mode == TTY_MODE_RAW) {
        // Raw: терминал не вмешивается. Байт отдаётся как есть; собирается
        // только UTF-8, чтобы вызывающий видел символ, а не два байта. Строки
        // и эха здесь нет по определению - это и есть отличие от канона.
        if (byte < 0x80) {
            utf8_reset(&s_utf8);
            return make(TTY_EV_CHAR, byte);
        }

        uint32_t cp = 0;
        const utf8_status st = utf8_feed(&s_utf8, byte, &cp);
        if (st == UTF8_DONE) {
            return make(TTY_EV_CHAR, cp);
        }
        // Недособранная буква и битый байт - наружу не отдаём: половинка
        // символа программе бесполезна.
        return make(TTY_EV_NONE, 0);
    }

    // --- канонический режим ---

    // Ждали продолжение буквы, а пришёл не хвост: последовательность битая.
    // Проверка стоит до разбора байта, потому что откатывать надо и перед
    // Enter, и перед ASCII, и перед управляющим - иначе огрызок попадёт в
    // готовую строку.
    if ((byte & 0xC0u) != 0x80u) {
        drop_partial();
    }

    if (byte == LF || byte == CR) {
        s_line[s_len] = '\0';
        s_ready = true;
        return make(TTY_EV_LINE, 0);
    }

    if (byte == BS || byte == DEL) {
        // 0x7F (DEL) - это то, что присылает клавиша Backspace в терминале
        // мака; 0x08 - то, что присылали терминалы раньше. Принимаем оба,
        // иначе Backspace работал бы или там, или там.
        if (s_len == 0) {
            return make(TTY_EV_BELL, 0);
        }
        trim_last_cp();
        utf8_reset(&s_utf8);
        return make(TTY_EV_ERASE, 0);
    }

    if (byte == CTRL_D) {
        // Устоявшееся поведение: Ctrl-D на непустой строке отдаёт накопленное,
        // а на пустой означает конец ввода. Иначе набирать «выйти» пришлось бы
        // отдельной командой, которой ещё нет.
        if (s_len == 0) {
            return make(TTY_EV_EOF, 0);
        }
        s_line[s_len] = '\0';
        s_ready = true;
        return make(TTY_EV_LINE, 0);
    }

    if (byte < 0x20) {
        // Прочие управляющие пока не поддерживаются. Пропускаем молча: у нас
        // нет ни истории (стрелки приходят как ESC-последовательности), ни
        // Ctrl-U, и делать вид, что нажатие сработало, нельзя.
        return make(TTY_EV_NONE, 0);
    }

    if (byte < 0x80) {
        utf8_reset(&s_utf8);
        if (!push_byte(byte)) {
            return make(TTY_EV_BELL, 0);
        }
        return make(TTY_EV_CHAR, byte);
    }

    return input_multibyte(byte);
}
