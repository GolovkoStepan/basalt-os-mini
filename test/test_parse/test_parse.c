// Тесты разбора командной строки на хосте.
//
// Почему здесь: правила кавычек и экранирования - источник редких и обидных
// ошибок («путь с пробелом работает через раз»). На плате каждую проверку
// пришлось бы набирать руками, а тут она стоит строки.

#include <unity.h>

#include <string.h>

#include "parse.h"

void setUp(void)
{
}

void tearDown(void)
{
}

// Разобрать изменяемую копию строки: parse_line правит буфер на месте.
// Статический буфер, а не локальный: argv указывает внутрь него, и тест должен
// видеть результат после возврата из feed - как и шелл.
static char s_buf[256];
static parse_result s_res;

static parse_status feed(const char* line)
{
    strncpy(s_buf, line, sizeof(s_buf) - 1);
    s_buf[sizeof(s_buf) - 1] = '\0';
    memset(&s_res, 0, sizeof(s_res));
    return parse_line(s_buf, &s_res);
}

void test_empty_line(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed(""));
    TEST_ASSERT_EQUAL_INT(0, s_res.argc);
}

void test_only_spaces(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("   \t  "));
    TEST_ASSERT_EQUAL_INT(0, s_res.argc);
}

void test_single_word(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("help"));
    TEST_ASSERT_EQUAL_INT(1, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("help", s_res.argv[0]);
}

void test_many_words(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo privet mir"));
    TEST_ASSERT_EQUAL_INT(3, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("echo", s_res.argv[0]);
    TEST_ASSERT_EQUAL_STRING("privet", s_res.argv[1]);
    TEST_ASSERT_EQUAL_STRING("mir", s_res.argv[2]);
}

void test_many_spaces_between_words(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("  a \t\t b   c "));
    TEST_ASSERT_EQUAL_INT(3, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a", s_res.argv[0]);
    TEST_ASSERT_EQUAL_STRING("b", s_res.argv[1]);
    TEST_ASSERT_EQUAL_STRING("c", s_res.argv[2]);
}

void test_double_quotes_keep_space(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo \"a b\" c"));
    TEST_ASSERT_EQUAL_INT(3, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("echo", s_res.argv[0]);
    TEST_ASSERT_EQUAL_STRING("a b", s_res.argv[1]);
    TEST_ASSERT_EQUAL_STRING("c", s_res.argv[2]);
}

void test_single_quotes_are_literal(void)
{
    // В одинарных кавычках обратная косая черта - обычный символ.
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo 'a\\b $x'"));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a\\b $x", s_res.argv[1]);
}

void test_escaped_space(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo a\\ b"));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a b", s_res.argv[1]);
}

void test_escaped_quote_and_backslash(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo \"a\\\"b\\\\c\""));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a\"b\\c", s_res.argv[1]);
}

void test_backslash_inside_double_quotes_is_kept(void)
{
    // Экранирует только кавычку и себя: иначе путь в Windows-стиле терял бы
    // буквы, а такого пути у нас и быть не может, но правило должно быть
    // предсказуемым.
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo \"a\\nb\""));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a\\nb", s_res.argv[1]);
}

void test_quoted_and_bare_glue_together(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo ab\"c d\"ef"));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("abc def", s_res.argv[1]);
}

void test_empty_quotes_give_empty_word(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo \"\""));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("", s_res.argv[1]);
}

void test_empty_single_quotes_give_empty_word(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("''"));
    TEST_ASSERT_EQUAL_INT(1, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("", s_res.argv[0]);
}

void test_comment_starts_word(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo a # b c"));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a", s_res.argv[1]);
}

void test_comment_at_line_start(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("# только комментарий"));
    TEST_ASSERT_EQUAL_INT(0, s_res.argc);
}

void test_hash_inside_word_is_not_comment(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo a#b"));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a#b", s_res.argv[1]);
}

void test_hash_in_quotes_is_not_comment(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo \"a # b\""));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a # b", s_res.argv[1]);
}

void test_unterminated_double_quote(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_ERR_QUOTE, feed("echo \"a b"));
}

void test_unterminated_single_quote(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_ERR_QUOTE, feed("echo 'a"));
}

void test_too_many_args(void)
{
    char line[128];
    int at = 0;
    for (int i = 0; i <= PARSE_MAX_ARGS; ++i) {
        line[at++] = 'a';
        line[at++] = ' ';
    }
    line[at] = '\0';

    TEST_ASSERT_EQUAL_INT(PARSE_ERR_ARGS, feed(line));
}

void test_exactly_max_args(void)
{
    char line[128];
    int at = 0;
    for (int i = 0; i < PARSE_MAX_ARGS; ++i) {
        line[at++] = 'a';
        line[at++] = ' ';
    }
    line[at] = '\0';

    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed(line));
    TEST_ASSERT_EQUAL_INT(PARSE_MAX_ARGS, s_res.argc);
}

void test_trailing_backslash_is_literal(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo a\\"));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a\\", s_res.argv[1]);
}

void test_cyrillic_passes_through(void)
{
    // «Ж» в UTF-8 - два байта. Парсер про них ничего не знает и не должен:
    // это строка байтов, а не символов.
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo привет"));
    TEST_ASSERT_EQUAL_INT(2, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("привет", s_res.argv[1]);
}

// ---------------------------------------------------------------------------
// Служебные токены
// ---------------------------------------------------------------------------

void test_out_is_a_token(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo a > f"));
    TEST_ASSERT_EQUAL_INT(4, s_res.argc);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_WORD, s_res.kind[0]);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_WORD, s_res.kind[1]);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_OUT, s_res.kind[2]);
    TEST_ASSERT_EQUAL_STRING(">", s_res.argv[2]);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_WORD, s_res.kind[3]);
}

void test_append_is_one_token(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo a >> f"));
    TEST_ASSERT_EQUAL_INT(4, s_res.argc);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_APPEND, s_res.kind[2]);
    TEST_ASSERT_EQUAL_STRING(">>", s_res.argv[2]);
}

void test_in_and_pipe_are_tokens(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("cat < f | ls"));
    TEST_ASSERT_EQUAL_INT(5, s_res.argc);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_IN, s_res.kind[1]);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_PIPE, s_res.kind[3]);
    TEST_ASSERT_EQUAL_STRING("|", s_res.argv[3]);
}

void test_glued_redirect_splits_words(void)
{
    // `a>f` без пробелов: слово, токен, слово. И следующее слово не съедено:
    // служебный текст лежит вне буфера, поэтому ноль не затирает `f`.
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo a>f"));
    TEST_ASSERT_EQUAL_INT(4, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a", s_res.argv[1]);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_OUT, s_res.kind[2]);
    TEST_ASSERT_EQUAL_STRING("f", s_res.argv[3]);
}

void test_quoted_redirect_is_a_word(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo '>' \"<\""));
    TEST_ASSERT_EQUAL_INT(3, s_res.argc);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_WORD, s_res.kind[1]);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_WORD, s_res.kind[2]);
    TEST_ASSERT_EQUAL_STRING(">", s_res.argv[1]);
    TEST_ASSERT_EQUAL_STRING("<", s_res.argv[2]);
}

void test_escaped_redirect_is_a_word(void)
{
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo \\| \\< \\>"));
    TEST_ASSERT_EQUAL_INT(4, s_res.argc);
    for (int i = 1; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT(PARSE_TOK_WORD, s_res.kind[i]);
    }
    TEST_ASSERT_EQUAL_STRING("|", s_res.argv[1]);
    TEST_ASSERT_EQUAL_STRING("<", s_res.argv[2]);
    TEST_ASSERT_EQUAL_STRING(">", s_res.argv[3]);
}

void test_words_around_redirect_keep_order(void)
{
    // Сжатие буфера не должно путать порядок: путь идёт за своим токеном.
    TEST_ASSERT_EQUAL_INT(PARSE_OK, feed("echo \"a b\" > f g"));
    TEST_ASSERT_EQUAL_INT(5, s_res.argc);
    TEST_ASSERT_EQUAL_STRING("a b", s_res.argv[1]);
    TEST_ASSERT_EQUAL_INT(PARSE_TOK_OUT, s_res.kind[2]);
    TEST_ASSERT_EQUAL_STRING("f", s_res.argv[3]);
    TEST_ASSERT_EQUAL_STRING("g", s_res.argv[4]);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_empty_line);
    RUN_TEST(test_only_spaces);
    RUN_TEST(test_single_word);
    RUN_TEST(test_many_words);
    RUN_TEST(test_many_spaces_between_words);

    RUN_TEST(test_double_quotes_keep_space);
    RUN_TEST(test_single_quotes_are_literal);
    RUN_TEST(test_escaped_space);
    RUN_TEST(test_escaped_quote_and_backslash);
    RUN_TEST(test_backslash_inside_double_quotes_is_kept);
    RUN_TEST(test_quoted_and_bare_glue_together);
    RUN_TEST(test_empty_quotes_give_empty_word);
    RUN_TEST(test_empty_single_quotes_give_empty_word);

    RUN_TEST(test_comment_starts_word);
    RUN_TEST(test_comment_at_line_start);
    RUN_TEST(test_hash_inside_word_is_not_comment);
    RUN_TEST(test_hash_in_quotes_is_not_comment);

    RUN_TEST(test_unterminated_double_quote);
    RUN_TEST(test_unterminated_single_quote);
    RUN_TEST(test_too_many_args);
    RUN_TEST(test_exactly_max_args);
    RUN_TEST(test_trailing_backslash_is_literal);
    RUN_TEST(test_cyrillic_passes_through);

    RUN_TEST(test_out_is_a_token);
    RUN_TEST(test_append_is_one_token);
    RUN_TEST(test_in_and_pipe_are_tokens);
    RUN_TEST(test_glued_redirect_splits_words);
    RUN_TEST(test_quoted_redirect_is_a_word);
    RUN_TEST(test_escaped_redirect_is_a_word);
    RUN_TEST(test_words_around_redirect_keep_order);

    return UNITY_END();
}
