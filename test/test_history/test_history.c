// Тесты кольца истории на хосте.
//
// Почему отдельным набором: у истории своя политика (что хранить, что
// выбрасывать, что делать при переполнении), и проверить её проще здесь, чем
// на плате, где «не та команда в ↑» выглядит как ошибка редактора.

#include <unity.h>

#include <string.h>

#include "history.h"

void setUp(void)
{
    history_init();
}

void tearDown(void)
{
}

// Положить строку и убедиться, что её приняли.
static void push_ok(const char* s)
{
    TEST_ASSERT_TRUE(history_push(s));
}

void test_empty_after_init(void)
{
    TEST_ASSERT_EQUAL_INT(0, history_count());
    TEST_ASSERT_EQUAL_STRING("", history_get(0));
}

void test_push_one(void)
{
    push_ok("help");
    TEST_ASSERT_EQUAL_INT(1, history_count());
    TEST_ASSERT_EQUAL_STRING("help", history_get(0));
}

void test_order_oldest_first(void)
{
    push_ok("a");
    push_ok("b");
    push_ok("c");

    TEST_ASSERT_EQUAL_INT(3, history_count());
    TEST_ASSERT_EQUAL_STRING("a", history_get(0));
    TEST_ASSERT_EQUAL_STRING("b", history_get(1));
    TEST_ASSERT_EQUAL_STRING("c", history_get(2));
}

void test_empty_line_not_stored(void)
{
    TEST_ASSERT_FALSE(history_push(""));
    TEST_ASSERT_FALSE(history_push(NULL));
    TEST_ASSERT_EQUAL_INT(0, history_count());
}

void test_duplicate_of_last_not_stored(void)
{
    push_ok("free");
    TEST_ASSERT_FALSE(history_push("free"));
    TEST_ASSERT_EQUAL_INT(1, history_count());
}

void test_duplicate_of_older_is_stored(void)
{
    push_ok("free");
    push_ok("top");
    push_ok("free");

    TEST_ASSERT_EQUAL_INT(3, history_count());
    TEST_ASSERT_EQUAL_STRING("free", history_get(2));
}

void test_overflow_keeps_newest(void)
{
    char line[16];
    for (int i = 0; i < HISTORY_CAP + 2; ++i) {
        line[0] = 'c';
        line[1] = (char)('0' + i);
        line[2] = '\0';
        push_ok(line);
    }

    TEST_ASSERT_EQUAL_INT(HISTORY_CAP, history_count());
    // Две самые старые записи вытеснены: первой должна быть c2.
    TEST_ASSERT_EQUAL_STRING("c2", history_get(0));
    TEST_ASSERT_EQUAL_STRING("c9", history_get(HISTORY_CAP - 1));
}

void test_too_long_rejected(void)
{
    char line[HISTORY_LINE_CAP + 8];
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    TEST_ASSERT_FALSE(history_push(line));
    TEST_ASSERT_EQUAL_INT(0, history_count());
}

void test_longest_allowed(void)
{
    char line[HISTORY_LINE_CAP];
    memset(line, 'x', HISTORY_LINE_CAP - 1);
    line[HISTORY_LINE_CAP - 1] = '\0';

    TEST_ASSERT_TRUE(history_push(line));
    TEST_ASSERT_EQUAL_STRING(line, history_get(0));
}

void test_get_out_of_range_is_empty(void)
{
    push_ok("one");

    TEST_ASSERT_EQUAL_STRING("", history_get(-1));
    TEST_ASSERT_EQUAL_STRING("", history_get(1));
    TEST_ASSERT_EQUAL_STRING("", history_get(100));
}

void test_clear(void)
{
    push_ok("one");
    push_ok("two");

    history_clear();

    TEST_ASSERT_EQUAL_INT(0, history_count());
    TEST_ASSERT_EQUAL_STRING("", history_get(0));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_empty_after_init);
    RUN_TEST(test_push_one);
    RUN_TEST(test_order_oldest_first);
    RUN_TEST(test_empty_line_not_stored);
    RUN_TEST(test_duplicate_of_last_not_stored);
    RUN_TEST(test_duplicate_of_older_is_stored);
    RUN_TEST(test_overflow_keeps_newest);
    RUN_TEST(test_too_long_rejected);
    RUN_TEST(test_longest_allowed);
    RUN_TEST(test_get_out_of_range_is_empty);
    RUN_TEST(test_clear);

    return UNITY_END();
}
