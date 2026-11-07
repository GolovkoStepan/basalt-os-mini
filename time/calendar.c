// Календарь: арифметика дат. Устройство и причины - в calendar.h.

#include "calendar.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Дни и годы
//
// Дата раскладывается не «в лоб» по месяцам в цикле, а эрой (400 лет) и днём
// внутри эры: тогда нет ни одной ветки на высокосность, а ошибка високосного
// года не может появиться отдельно от ошибки месяца. Формулы - те же, что
// в календаре C++20 (civil_from_days), и это не совпадение: они проверены
// на всём диапазоне лет, а не на паре удобных дат.
// ---------------------------------------------------------------------------

// 1970-01-01 в этой системе счёта - день 0. Начало отсчёта выбрано так, чтобы
// 1 марта было началом года: тогда 29 февраля всегда попадает в конец года,
// и лишний день не рвёт разбиение по месяцам.
#define UNIX_EPOCH_SHIFT 719468

// Пределы, которые даёт 32-битное число секунд. За ними календарь формально
// работает, но прошивке это не нужно, а отказ честнее переполнения.
#define UNIX_MAX_YEAR 2106
#define UNIX_MIN_YEAR 1970

static int floor_div(int a, int b)
{
    // В C деление округляет к нулю, а эра требует округления вниз: без этого
    // даты до 1970-01-01 уезжали бы на день.
    const int q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

static int32_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    const int era = floor_div(y, 400);
    const int yoe = y - era * 400;                              // 0..399
    const int doy = (153 * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + doe - UNIX_EPOCH_SHIFT);
}

static void civil_from_days(int32_t z, int* y, int* m, int* d)
{
    z += UNIX_EPOCH_SHIFT;
    const int era = floor_div((int)z, 146097);
    const int doe = (int)z - era * 146097;                       // 0..146096
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int year = yoe + era * 400;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;                          // 0..11 от марта
    const int day = doy - (153 * mp + 2) / 5 + 1;
    const int month = mp + ((mp < 10) ? 3 : -9);

    *y = year + ((month <= 2) ? 1 : 0);
    *m = month;
    *d = day;
}

// Сколько дней в месяце. Февраль считается по году, а не по таблице: таблица
// из двенадцати чисел и проверка на 29-е - это два места, которые могут
// разойтись, а формула одна.
static int days_in_month(int y, int m)
{
    static const uint8_t s_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) {
        return 0;
    }
    if (m != 2) {
        return s_days[m - 1];
    }
    const bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    return leap ? 29 : 28;
}

void cal_from_unix(uint32_t unix_secs, cal_t* out)
{
    if (out == NULL) {
        return;
    }
    const int32_t days = (int32_t)(unix_secs / 86400u);
    uint32_t rest = unix_secs % 86400u;

    civil_from_days(days, &out->year, &out->month, &out->day);
    out->hour = (int)(rest / 3600u);
    rest %= 3600u;
    out->min = (int)(rest / 60u);
    out->sec = (int)(rest % 60u);

    // 1970-01-01 был четвергом, а нулём у нас понедельник.
    out->wday = (int)(((days % 7) + 7 + 3) % 7);
}

bool cal_to_unix(const cal_t* t, uint32_t* out)
{
    if (t == NULL || out == NULL) {
        return false;
    }
    if (t->year < UNIX_MIN_YEAR || t->year > UNIX_MAX_YEAR) {
        return false;
    }
    if (t->month < 1 || t->month > 12) {
        return false;
    }
    if (t->day < 1 || t->day > days_in_month(t->year, t->month)) {
        return false;
    }
    if (t->hour < 0 || t->hour > 23 || t->min < 0 || t->min > 59 || t->sec < 0 ||
        t->sec > 59) {
        return false;
    }

    const int32_t days = days_from_civil(t->year, t->month, t->day);
    const int64_t secs = (int64_t)days * 86400 + t->hour * 3600 + t->min * 60 + t->sec;
    if (secs < 0 || secs > (int64_t)0xFFFFFFFFu) {
        return false;
    }
    *out = (uint32_t)secs;
    return true;
}

int cal_weekday(uint32_t unix_secs)
{
    // Считается от дней, а не от полной структуры: день недели не зависит от
    // времени суток, и гонять ради него разбор часов незачем.
    const int32_t days = (int32_t)(unix_secs / 86400u);
    return (int)(((days % 7) + 7 + 3) % 7);
}

const char* cal_weekday_short(int wday)
{
    static const char* const s_names[7] = {"пн", "вт", "ср", "чт", "пт", "сб", "вс"};
    if (wday < 0 || wday > 6) {
        return "??";
    }
    return s_names[wday];
}

// ---------------------------------------------------------------------------
// Печать
//
// Свой формат без printf: во-первых, printf тянет newlib ради одной строки,
// во-вторых, на экране нужен фиксированный вид, и «12» вместо «02» ломало бы
// колонку дат в `ls -l`.
// ---------------------------------------------------------------------------

static size_t put2(char* buf, size_t cap, size_t at, int v)
{
    if (at + 2 >= cap) {
        return at;
    }
    buf[at++] = (char)('0' + (v / 10) % 10);
    buf[at++] = (char)('0' + v % 10);
    return at;
}

static size_t put4(char* buf, size_t cap, size_t at, int v)
{
    if (v < 0) {
        v = 0;
    }
    if (at + 4 >= cap) {
        return at;
    }
    buf[at++] = (char)('0' + (v / 1000) % 10);
    buf[at++] = (char)('0' + (v / 100) % 10);
    buf[at++] = (char)('0' + (v / 10) % 10);
    buf[at++] = (char)('0' + v % 10);
    return at;
}

static size_t put_char(char* buf, size_t cap, size_t at, char c)
{
    if (at + 1 >= cap) {
        return at;
    }
    buf[at++] = c;
    return at;
}

static void zero_tail(char* buf, size_t cap, size_t at)
{
    if (cap != 0) {
        buf[(at < cap) ? at : cap - 1] = '\0';
    }
}

const char* cal_text(char* buf, size_t cap, uint32_t unix_secs)
{
    if (buf == NULL || cap == 0) {
        return buf;
    }
    cal_t t;
    cal_from_unix(unix_secs, &t);

    size_t at = put4(buf, cap, 0, t.year);
    at = put_char(buf, cap, at, '-');
    at = put2(buf, cap, at, t.month);
    at = put_char(buf, cap, at, '-');
    at = put2(buf, cap, at, t.day);
    at = put_char(buf, cap, at, ' ');
    at = put2(buf, cap, at, t.hour);
    at = put_char(buf, cap, at, ':');
    at = put2(buf, cap, at, t.min);
    at = put_char(buf, cap, at, ':');
    at = put2(buf, cap, at, t.sec);
    zero_tail(buf, cap, at);
    return buf;
}

const char* cal_text_iso(char* buf, size_t cap, uint32_t unix_secs)
{
    const char* const s = cal_text(buf, cap, unix_secs);
    if (buf != NULL) {
        // Десятый символ - разделитель даты и времени. Проверка на длину
        // нужна потому, что при коротком буфере его там может не быть.
        const size_t n = (cap == 0) ? 0 : strlen(buf);
        if (n > 10) {
            buf[10] = 'T';
        }
    }
    return s;
}

const char* cal_text_short(char* buf, size_t cap, uint32_t unix_secs)
{
    if (buf == NULL || cap == 0) {
        return buf;
    }
    cal_t t;
    cal_from_unix(unix_secs, &t);

    size_t at = put2(buf, cap, 0, t.day);
    at = put_char(buf, cap, at, '.');
    at = put2(buf, cap, at, t.month);
    at = put_char(buf, cap, at, ' ');
    at = put2(buf, cap, at, t.hour);
    at = put_char(buf, cap, at, ':');
    at = put2(buf, cap, at, t.min);
    zero_tail(buf, cap, at);
    return buf;
}

// ---------------------------------------------------------------------------
// Разбор
//
// Формат один: "YYYY-MM-DD" затем разделитель (T или пробел) и "HH:MM" с
// необязательными секундами. Не «гибкий», а именно один: стоит разрешить
// "07.11.2026", и однажды окажется, что половина файлов на плате в одном
// порядке, а половина в другом. Ошибка ввода здесь дешевле неоднозначности.
// ---------------------------------------------------------------------------

static bool digits(const char** p, int n, int* out)
{
    int v = 0;
    for (int i = 0; i < n; ++i) {
        const char c = (*p)[i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    *p += n;
    *out = v;
    return true;
}

static bool expect(const char** p, char c)
{
    if (**p != c) {
        return false;
    }
    ++*p;
    return true;
}

bool cal_parse(const char* text, uint32_t* unix_secs)
{
    if (text == NULL || unix_secs == NULL) {
        return false;
    }

    const char* p = text;
    cal_t t;
    memset(&t, 0, sizeof(t));

    if (!digits(&p, 4, &t.year) || !expect(&p, '-') || !digits(&p, 2, &t.month) ||
        !expect(&p, '-') || !digits(&p, 2, &t.day)) {
        return false;
    }
    if (*p != 'T' && *p != ' ') {
        return false;
    }
    ++p;

    if (!digits(&p, 2, &t.hour) || !expect(&p, ':') || !digits(&p, 2, &t.min)) {
        return false;
    }
    if (*p == ':') {
        ++p;
        if (!digits(&p, 2, &t.sec)) {
            return false;
        }
    }

    // Хвост: только пробельные символы. Перевод строки приходит из файла,
    // \r - из файла, собранного на маке; всё остальное значит, что в строке
    // не дата, а что-то ещё, и догадываться об этом не нужно.
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (*p != '\0') {
        return false;
    }

    return cal_to_unix(&t, unix_secs);
}
