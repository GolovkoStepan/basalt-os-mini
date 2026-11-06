// Тесты плана выполнения строки: перенаправления и конвейер.
//
// Почему на маке: решение «куда идёт вывод» проверяется сравнением указателей
// и чисел, и это в сто раз дешевле, чем ловить «echo не туда написал» по логу
// платы. Здесь же проверяются случаи, которые на плате набирать жалко: `>`
// без файла, два `>` в одной ступени, пустая ступень конвейера.

#include <unity.h>

#include <string.h>

#include "parse.h"
#include "redirect.h"

void setUp(void)
{
}

void tearDown(void)
{
}

// Строка и результат статические: argv и пути указывают внутрь буфера и
// должны пережить возврат из feed - как в шелле.
static char s_buf[256];
static parse_result s_parsed;
static redirect_plan s_plan;

static redirect_status feed(const char* line)
{
    strncpy(s_buf, line, sizeof(s_buf) - 1);
    s_buf[sizeof(s_buf) - 1] = '\0';
    memset(&s_parsed, 0, sizeof(s_parsed));
    memset(&s_plan, 0, sizeof(s_plan));

    if (parse_line(s_buf, &s_parsed) != PARSE_OK) {
        return REDIRECT_ERR_NO_COMMAND;  // разбор не про перенаправления
    }
    return redirect_build(&s_parsed, &s_plan);
}

static void assert_stage(int index, int argc)
{
    TEST_ASSERT_EQUAL_INT(argc, s_plan.stage[index].argc);
}

// ---------------------------------------------------------------------------
// Без перенаправлений
// ---------------------------------------------------------------------------

void test_plain_command_is_one_stage(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("ls /etc"));
    TEST_ASSERT_EQUAL_INT(1, s_plan.count);
    assert_stage(0, 2);
    TEST_ASSERT_EQUAL_STRING("ls", s_plan.stage[0].argv[0]);
    TEST_ASSERT_EQUAL_STRING("/etc", s_plan.stage[0].argv[1]);
    TEST_ASSERT_NULL(s_plan.stage[0].out_path);
    TEST_ASSERT_NULL(s_plan.stage[0].in_path);
}

// ---------------------------------------------------------------------------
// Вывод
// ---------------------------------------------------------------------------

void test_out_redirect(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("echo a > f"));
    TEST_ASSERT_EQUAL_INT(1, s_plan.count);
    assert_stage(0, 2);
    TEST_ASSERT_EQUAL_STRING("echo", s_plan.stage[0].argv[0]);
    TEST_ASSERT_EQUAL_STRING("a", s_plan.stage[0].argv[1]);
    TEST_ASSERT_EQUAL_STRING("f", s_plan.stage[0].out_path);
    TEST_ASSERT_FALSE(s_plan.stage[0].append);
}

void test_append_redirect(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("echo a >> f"));
    TEST_ASSERT_EQUAL_STRING("f", s_plan.stage[0].out_path);
    TEST_ASSERT_TRUE(s_plan.stage[0].append);
}

void test_out_redirect_glued_to_word(void)
{
    // Пробелы не обязательны: `echo a>f` - то же самое.
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("echo a>f"));
    assert_stage(0, 2);
    TEST_ASSERT_EQUAL_STRING("a", s_plan.stage[0].argv[1]);
    TEST_ASSERT_EQUAL_STRING("f", s_plan.stage[0].out_path);
}

void test_redirect_before_command(void)
{
    // Как в sh: порядок не важен, команда - это оставшиеся слова.
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("> f echo a"));
    assert_stage(0, 2);
    TEST_ASSERT_EQUAL_STRING("echo", s_plan.stage[0].argv[0]);
    TEST_ASSERT_EQUAL_STRING("f", s_plan.stage[0].out_path);
}

void test_redirect_in_the_middle_keeps_later_words(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("echo a > f b"));
    assert_stage(0, 3);
    TEST_ASSERT_EQUAL_STRING("a", s_plan.stage[0].argv[1]);
    TEST_ASSERT_EQUAL_STRING("b", s_plan.stage[0].argv[2]);
    TEST_ASSERT_EQUAL_STRING("f", s_plan.stage[0].out_path);
}

void test_out_without_target(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_NO_TARGET, feed("echo a >"));
}

void test_out_followed_by_redirect(void)
{
    // `> > f` - имя файла не может быть управляющим символом.
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_NO_TARGET, feed("echo a > > f"));
}

void test_double_out_is_error(void)
{
    // В sh победил бы последний, а первый файл остался бы пустым. Молча
    // созданный пустой файл - худший из возможных ответов.
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_DOUBLE_OUT, feed("echo a > f > g"));
}

void test_out_and_append_are_the_same_stream(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_DOUBLE_OUT, feed("echo a > f >> g"));
}

// ---------------------------------------------------------------------------
// Ввод
// ---------------------------------------------------------------------------

void test_in_redirect(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("cat < f"));
    assert_stage(0, 1);
    TEST_ASSERT_EQUAL_STRING("cat", s_plan.stage[0].argv[0]);
    TEST_ASSERT_EQUAL_STRING("f", s_plan.stage[0].in_path);
    TEST_ASSERT_NULL(s_plan.stage[0].out_path);
}

void test_in_and_out_together(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("cat < f > g"));
    TEST_ASSERT_EQUAL_STRING("f", s_plan.stage[0].in_path);
    TEST_ASSERT_EQUAL_STRING("g", s_plan.stage[0].out_path);
}

void test_double_in_is_error(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_DOUBLE_IN, feed("cat < f < g"));
}

void test_in_without_target(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_NO_TARGET, feed("cat <"));
}

// ---------------------------------------------------------------------------
// Конвейер
// ---------------------------------------------------------------------------

void test_pipe_splits_stages(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("cat f | grep a"));
    TEST_ASSERT_EQUAL_INT(2, s_plan.count);
    assert_stage(0, 2);
    assert_stage(1, 2);
    TEST_ASSERT_EQUAL_STRING("cat", s_plan.stage[0].argv[0]);
    TEST_ASSERT_EQUAL_STRING("grep", s_plan.stage[1].argv[0]);
    TEST_ASSERT_EQUAL_STRING("a", s_plan.stage[1].argv[1]);
}

void test_pipe_and_redirect_together(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("cat f | grep a > out"));
    TEST_ASSERT_EQUAL_INT(2, s_plan.count);
    TEST_ASSERT_NULL(s_plan.stage[0].out_path);
    TEST_ASSERT_EQUAL_STRING("out", s_plan.stage[1].out_path);
    assert_stage(1, 2);
}

void test_pipe_glued_to_words(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("cat f|grep a"));
    TEST_ASSERT_EQUAL_INT(2, s_plan.count);
    assert_stage(0, 2);
    assert_stage(1, 2);
}

void test_pipe_at_start_is_error(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_NO_COMMAND, feed("| grep a"));
}

void test_pipe_at_end_is_error(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_NO_COMMAND, feed("cat f |"));
}

void test_empty_stage_in_the_middle_is_error(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_NO_COMMAND, feed("cat f | | grep a"));
}

void test_too_many_stages(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_ERR_STAGES, feed("a | b | c | d"));
}

void test_max_stages_fit(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("a | b | c"));
    TEST_ASSERT_EQUAL_INT(REDIRECT_MAX_STAGES, s_plan.count);
}

// ---------------------------------------------------------------------------
// Кавычки и экранирование
// ---------------------------------------------------------------------------

void test_quoted_redirect_stays_a_word(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("echo '>' f"));
    TEST_ASSERT_EQUAL_INT(1, s_plan.count);
    assert_stage(0, 3);
    TEST_ASSERT_EQUAL_STRING(">", s_plan.stage[0].argv[1]);
    TEST_ASSERT_NULL(s_plan.stage[0].out_path);
}

void test_redirect_inside_quotes_is_text(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("echo \"a > b\""));
    assert_stage(0, 2);
    TEST_ASSERT_EQUAL_STRING("a > b", s_plan.stage[0].argv[1]);
    TEST_ASSERT_NULL(s_plan.stage[0].out_path);
}

void test_escaped_redirect_is_a_word(void)
{
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("echo \\> f"));
    assert_stage(0, 3);
    TEST_ASSERT_EQUAL_STRING(">", s_plan.stage[0].argv[1]);
    TEST_ASSERT_NULL(s_plan.stage[0].out_path);
}

void test_out_path_may_be_quoted(void)
{
    // Имя файла с пробелом: кавычки снимает разбор, путь приходит целым.
    TEST_ASSERT_EQUAL_INT(REDIRECT_OK, feed("echo a > \"my file\""));
    TEST_ASSERT_EQUAL_STRING("my file", s_plan.stage[0].out_path);
}

// ---------------------------------------------------------------------------
// Тексты ошибок
// ---------------------------------------------------------------------------

void test_strerror_is_never_empty(void)
{
    for (int st = REDIRECT_OK; st <= REDIRECT_ERR_STAGES; ++st) {
        TEST_ASSERT_TRUE(redirect_strerror((redirect_status)st)[0] != '\0' ||
                         st == REDIRECT_OK);
    }
    TEST_ASSERT_TRUE(redirect_strerror((redirect_status)-99)[0] != '\0');
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_plain_command_is_one_stage);

    RUN_TEST(test_out_redirect);
    RUN_TEST(test_append_redirect);
    RUN_TEST(test_out_redirect_glued_to_word);
    RUN_TEST(test_redirect_before_command);
    RUN_TEST(test_redirect_in_the_middle_keeps_later_words);
    RUN_TEST(test_out_without_target);
    RUN_TEST(test_out_followed_by_redirect);
    RUN_TEST(test_double_out_is_error);
    RUN_TEST(test_out_and_append_are_the_same_stream);

    RUN_TEST(test_in_redirect);
    RUN_TEST(test_in_and_out_together);
    RUN_TEST(test_double_in_is_error);
    RUN_TEST(test_in_without_target);

    RUN_TEST(test_pipe_splits_stages);
    RUN_TEST(test_pipe_and_redirect_together);
    RUN_TEST(test_pipe_glued_to_words);
    RUN_TEST(test_pipe_at_start_is_error);
    RUN_TEST(test_pipe_at_end_is_error);
    RUN_TEST(test_empty_stage_in_the_middle_is_error);
    RUN_TEST(test_too_many_stages);
    RUN_TEST(test_max_stages_fit);

    RUN_TEST(test_quoted_redirect_stays_a_word);
    RUN_TEST(test_redirect_inside_quotes_is_text);
    RUN_TEST(test_escaped_redirect_is_a_word);
    RUN_TEST(test_out_path_may_be_quoted);

    RUN_TEST(test_strerror_is_never_empty);

    return UNITY_END();
}
