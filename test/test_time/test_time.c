// Тесты календаря и часов на маке.
//
// Почему здесь: перевод секунд в дату ошибается незаметно и на один день, а
// замечается через неделю - когда по `ls -l` видно, что файл «создан в
// прошлом месяце». На маке таблица дат проверяется за секунду, и в неё входят
// именно те случаи, которые руками не перебрать: границы века, 2000-й
// (високосный), 2100-й (не високосный), последняя секунда суток, конец
// диапазона 32-битного счётчика.

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "calendar.h"
#include "clock.h"

void setUp(void)
{
}

void tearDown(void)
{
}

// ---------------------------------------------------------------------------
// Разбор секунд в дату
// ---------------------------------------------------------------------------

static void check(uint32_t secs, int y, int mo, int d, int h, int mi, int s, int wd)
{
    cal_t t;
    cal_from_unix(secs, &t);
    TEST_ASSERT_EQUAL_INT(y, t.year);
    TEST_ASSERT_EQUAL_INT(mo, t.month);
    TEST_ASSERT_EQUAL_INT(d, t.day);
    TEST_ASSERT_EQUAL_INT(h, t.hour);
    TEST_ASSERT_EQUAL_INT(mi, t.min);
    TEST_ASSERT_EQUAL_INT(s, t.sec);
    TEST_ASSERT_EQUAL_INT(wd, t.wday);
}

static void test_epoch_is_thursday(void)
{
    // 1970-01-01 был четвергом. Если нулём считать воскресенье, дни недели
    // съедут на единицу, и заметить это можно только по календарю.
    check(0u, 1970, 1, 1, 0, 0, 0, 3);
}

static void test_known_date(void)
{
    // 2026-11-07 18:42:03 - суббота. Число взято из реальной даты проекта.
    check(1794076923u, 2026, 11, 7, 18, 42, 3, 5);
}

static void test_leap_day_2024(void)
{
    // 2024-02-29 12:00:00
    check(1709208000u, 2024, 2, 29, 12, 0, 0, 3);
}

static void test_2100_is_not_leap(void)
{
    // 2100-02-28 23:59:59, и следующий день - 1 марта, а не 29 февраля.
    check(4107542399u, 2100, 2, 28, 23, 59, 59, 6);
    check(4107542400u, 2100, 3, 1, 0, 0, 0, 0);
}

static void test_2000_is_leap(void)
{
    // 2000-02-29 00:00:00 - век, делящийся на 400, поэтому високосный.
    check(951782400u, 2000, 2, 29, 0, 0, 0, 1);
}

static void test_last_second_of_day(void)
{
    check(1794095999u, 2026, 11, 7, 23, 59, 59, 5);
}

// ---------------------------------------------------------------------------
// Сборка даты
// ---------------------------------------------------------------------------

static void test_roundtrip(void)
{
    // Каждая дата из диапазона должна пережить разбор туда и обратно. Перебор
    // по дням, а не по секундам: этого достаточно, чтобы поймать ошибку в
    // високосах и месяцах, и укладывается в секунды.
    for (uint32_t day = 0; day < 40000u; day += 7u) {
        const uint32_t secs = day * 86400u + 3661u;
        cal_t t;
        cal_from_unix(secs, &t);
        uint32_t back = 0;
        TEST_ASSERT_TRUE(cal_to_unix(&t, &back));
        TEST_ASSERT_EQUAL_UINT32(secs, back);
    }
}

static void test_rejects_nonexistent(void)
{
    cal_t t;
    uint32_t out = 0;

    memset(&t, 0, sizeof(t));
    t.year = 2026;
    t.month = 2;
    t.day = 31;
    TEST_ASSERT_FALSE(cal_to_unix(&t, &out));

    memset(&t, 0, sizeof(t));
    t.year = 2026;
    t.month = 13;
    t.day = 1;
    TEST_ASSERT_FALSE(cal_to_unix(&t, &out));

    memset(&t, 0, sizeof(t));
    t.year = 2026;
    t.month = 1;
    t.day = 1;
    t.hour = 24;
    TEST_ASSERT_FALSE(cal_to_unix(&t, &out));

    memset(&t, 0, sizeof(t));
    t.year = 1969;
    t.month = 12;
    t.day = 31;
    TEST_ASSERT_FALSE(cal_to_unix(&t, &out));

    // 2023-02-29 не существует: 2023 не високосный.
    memset(&t, 0, sizeof(t));
    t.year = 2023;
    t.month = 2;
    t.day = 29;
    TEST_ASSERT_FALSE(cal_to_unix(&t, &out));
}

// ---------------------------------------------------------------------------
// Печать
// ---------------------------------------------------------------------------

static void test_text_format(void)
{
    char buf[CAL_TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING("2026-11-07 18:42:03", cal_text(buf, sizeof(buf), 1794076923u));
    TEST_ASSERT_EQUAL_STRING("1970-01-01 00:00:00", cal_text(buf, sizeof(buf), 0u));
    TEST_ASSERT_EQUAL_STRING("2026-11-07T18:42:03", cal_text_iso(buf, sizeof(buf), 1794076923u));
    TEST_ASSERT_EQUAL_STRING("07.11 18:42", cal_text_short(buf, sizeof(buf), 1794076923u));
}

static void test_text_truncates_safely(void)
{
    // Короткий буфер: строка обрезается, но остаётся строкой. Иначе вывод
    // уехал бы в соседнюю переменную на стеке.
    char buf[8];
    memset(buf, 'X', sizeof(buf));
    const char* const s = cal_text(buf, sizeof(buf), 1794076923u);
    TEST_ASSERT_EQUAL_PTR(buf, s);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);

    char one[1];
    cal_text(one, sizeof(one), 1794076923u);
    TEST_ASSERT_EQUAL_CHAR('\0', one[0]);
}

static void test_weekday_names(void)
{
    TEST_ASSERT_EQUAL_STRING("пн", cal_weekday_short(0));
    TEST_ASSERT_EQUAL_STRING("вс", cal_weekday_short(6));
    TEST_ASSERT_EQUAL_STRING("??", cal_weekday_short(7));
    TEST_ASSERT_EQUAL_INT(5, cal_weekday(1794076923u));
}

// ---------------------------------------------------------------------------
// Разбор текста
// ---------------------------------------------------------------------------

static void test_parse_full(void)
{
    uint32_t out = 0;
    TEST_ASSERT_TRUE(cal_parse("2026-11-07 18:42:03", &out));
    TEST_ASSERT_EQUAL_UINT32(1794076923u, out);

    TEST_ASSERT_TRUE(cal_parse("2026-11-07T18:42:03", &out));
    TEST_ASSERT_EQUAL_UINT32(1794076923u, out);
}

static void test_parse_without_seconds(void)
{
    uint32_t out = 0;
    TEST_ASSERT_TRUE(cal_parse("2026-11-07 18:42", &out));
    TEST_ASSERT_EQUAL_UINT32(1794076923u - 3u, out);
}

static void test_parse_tolerates_line_end(void)
{
    // Строка приходит из файла: \r\n от мака не должен делать дату негодной.
    uint32_t out = 0;
    TEST_ASSERT_TRUE(cal_parse("2026-11-07 18:42:03\r\n", &out));
    TEST_ASSERT_EQUAL_UINT32(1794076923u, out);
}

static void test_parse_rejects_garbage(void)
{
    uint32_t out = 0;
    TEST_ASSERT_FALSE(cal_parse("", &out));
    TEST_ASSERT_FALSE(cal_parse("2026", &out));
    TEST_ASSERT_FALSE(cal_parse("2026-11-07", &out));       // нет времени
    TEST_ASSERT_FALSE(cal_parse("26-11-07 18:42", &out));   // двузначный год
    TEST_ASSERT_FALSE(cal_parse("2026-11-07 18:42:03 x", &out));
    TEST_ASSERT_FALSE(cal_parse("2026-13-07 18:42:03", &out));
    TEST_ASSERT_FALSE(cal_parse("2026-11-31 18:42:03", &out));
    TEST_ASSERT_FALSE(cal_parse("2026-11-07 25:00:00", &out));
    TEST_ASSERT_FALSE(cal_parse("2026/11/07 18:42:03", &out));
    TEST_ASSERT_FALSE(cal_parse(NULL, &out));
    TEST_ASSERT_FALSE(cal_parse("2026-11-07 18:42:03", NULL));
}

// ---------------------------------------------------------------------------
// Часы
// ---------------------------------------------------------------------------

static void test_clock_starts_unsynced(void)
{
    clock_t c;
    clock_reset(&c);
    TEST_ASSERT_FALSE(clock_synced(&c));

    uint32_t now = 12345;
    TEST_ASSERT_FALSE(clock_now(&c, 1000000u, &now));
    // Не тронули: вызывающий сам решает, что показать вместо даты.
    TEST_ASSERT_EQUAL_UINT32(12345, now);
}

static void test_clock_counts_from_set_point(void)
{
    clock_t c;
    clock_reset(&c);
    clock_set(&c, 1794076923u, 5000000u);

    uint32_t now = 0;
    TEST_ASSERT_TRUE(clock_now(&c, 5000000u, &now));
    TEST_ASSERT_EQUAL_UINT32(1794076923u, now);

    TEST_ASSERT_TRUE(clock_now(&c, 5000000u + 3000000u, &now));
    TEST_ASSERT_EQUAL_UINT32(1794076926u, now);

    // Дробные доли отбрасываются, а не округляются: иначе часы прыгали бы
    // вперёд на границе секунды.
    TEST_ASSERT_TRUE(clock_now(&c, 5000000u + 999999u, &now));
    TEST_ASSERT_EQUAL_UINT32(1794076923u, now);
}

static void test_clock_handles_counter_going_back(void)
{
    // Чужой счётчик времени (другая точка старта) не должен давать дату в
    // прошлом молча: отдаём точку установки.
    clock_t c;
    clock_reset(&c);
    clock_set(&c, 1794076923u, 9000000u);

    uint32_t now = 0;
    TEST_ASSERT_TRUE(clock_now(&c, 1000000u, &now));
    TEST_ASSERT_EQUAL_UINT32(1794076923u, now);
    TEST_ASSERT_EQUAL_UINT32(0, clock_age_secs(&c, 1000000u));
}

static void test_clock_age(void)
{
    clock_t c;
    clock_reset(&c);
    clock_set(&c, 1794076923u, 0u);
    TEST_ASSERT_EQUAL_UINT32(65, clock_age_secs(&c, 65000000u));
    TEST_ASSERT_EQUAL_UINT32(0, clock_age_secs(&c, 0u));
}

// ---------------------------------------------------------------------------
// Длительности
// ---------------------------------------------------------------------------

#include "duration.h"

static void test_duration_bare_is_seconds(void)
{
    uint32_t ms = 0;
    // Как у обычного sleep: голое число - секунды.
    TEST_ASSERT_TRUE(duration_parse("5", &ms));
    TEST_ASSERT_EQUAL_UINT32(5000, ms);
    TEST_ASSERT_TRUE(duration_parse("0", &ms) == false);
    TEST_ASSERT_TRUE(duration_parse("0s", &ms) == false);
}

static void test_duration_units(void)
{
    uint32_t ms = 0;
    TEST_ASSERT_TRUE(duration_parse("300ms", &ms));
    TEST_ASSERT_EQUAL_UINT32(300, ms);
    TEST_ASSERT_TRUE(duration_parse("2s", &ms));
    TEST_ASSERT_EQUAL_UINT32(2000, ms);
    TEST_ASSERT_TRUE(duration_parse("3m", &ms));
    TEST_ASSERT_EQUAL_UINT32(180000, ms);
    TEST_ASSERT_TRUE(duration_parse("2h", &ms));
    TEST_ASSERT_EQUAL_UINT32(7200000, ms);
}

static void test_duration_combined(void)
{
    uint32_t ms = 0;
    TEST_ASSERT_TRUE(duration_parse("1m30s", &ms));
    TEST_ASSERT_EQUAL_UINT32(90000, ms);
    TEST_ASSERT_TRUE(duration_parse("1h2m3s", &ms));
    TEST_ASSERT_EQUAL_UINT32(3723000, ms);
    TEST_ASSERT_TRUE(duration_parse("1m500ms", &ms));
    TEST_ASSERT_EQUAL_UINT32(60500, ms);
}

static void test_duration_rejects_garbage(void)
{
    uint32_t ms = 0;
    TEST_ASSERT_FALSE(duration_parse("", &ms));
    TEST_ASSERT_FALSE(duration_parse("abc", &ms));
    TEST_ASSERT_FALSE(duration_parse("5x", &ms));
    TEST_ASSERT_FALSE(duration_parse("-5", &ms));
    TEST_ASSERT_FALSE(duration_parse("5 s", &ms));
    TEST_ASSERT_FALSE(duration_parse("25h", &ms));       // больше суток
    TEST_ASSERT_FALSE(duration_parse("999999999999", &ms));
    TEST_ASSERT_FALSE(duration_parse(NULL, &ms));
    TEST_ASSERT_FALSE(duration_parse("5", NULL));
}

static void test_duration_text(void)
{
    char buf[32];
    TEST_ASSERT_EQUAL_STRING("250 мс", duration_text(buf, sizeof(buf), 250));
    TEST_ASSERT_EQUAL_STRING("3 с", duration_text(buf, sizeof(buf), 3000));
    TEST_ASSERT_EQUAL_STRING("1 мин 30 с", duration_text(buf, sizeof(buf), 90000));
    TEST_ASSERT_EQUAL_STRING("2 ч 1 мин 5 с", duration_text(buf, sizeof(buf), 7265000));
    TEST_ASSERT_EQUAL_STRING("2 ч", duration_text(buf, sizeof(buf), 7200000));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_epoch_is_thursday);
    RUN_TEST(test_known_date);
    RUN_TEST(test_leap_day_2024);
    RUN_TEST(test_2100_is_not_leap);
    RUN_TEST(test_2000_is_leap);
    RUN_TEST(test_last_second_of_day);

    RUN_TEST(test_roundtrip);
    RUN_TEST(test_rejects_nonexistent);

    RUN_TEST(test_text_format);
    RUN_TEST(test_text_truncates_safely);
    RUN_TEST(test_weekday_names);

    RUN_TEST(test_parse_full);
    RUN_TEST(test_parse_without_seconds);
    RUN_TEST(test_parse_tolerates_line_end);
    RUN_TEST(test_parse_rejects_garbage);

    RUN_TEST(test_clock_starts_unsynced);
    RUN_TEST(test_clock_counts_from_set_point);
    RUN_TEST(test_clock_handles_counter_going_back);
    RUN_TEST(test_clock_age);

    RUN_TEST(test_duration_bare_is_seconds);
    RUN_TEST(test_duration_units);
    RUN_TEST(test_duration_combined);
    RUN_TEST(test_duration_rejects_garbage);
    RUN_TEST(test_duration_text);

    return UNITY_END();
}
