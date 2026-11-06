// Разбор командной строки. Правила и почему так - см. parse.h.

#include "parse.h"

#include <stdbool.h>

// Текст служебных токенов. Не в буфере строки: там на его месте может стоять
// следующий символ (`>f`), и запись нуля съела бы его.
static char s_tok_out[] = ">";
static char s_tok_append[] = ">>";
static char s_tok_in[] = "<";
static char s_tok_pipe[] = "|";

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

static bool is_special(char c)
{
    return c == '>' || c == '<' || c == '|';
}

// Вид и текст служебного токена по его первому символу. Длина - сколько
// байт занимает токен в исходной строке (`>>` - два).
static parse_token_kind special_kind(char c, const char* at, int* len)
{
    if (c == '>') {
        if (at[1] == '>') {
            *len = 2;
            return PARSE_TOK_APPEND;
        }
        *len = 1;
        return PARSE_TOK_OUT;
    }
    *len = 1;
    return (c == '<') ? PARSE_TOK_IN : PARSE_TOK_PIPE;
}

static char* special_text(parse_token_kind kind)
{
    switch (kind) {
    case PARSE_TOK_OUT:
        return s_tok_out;
    case PARSE_TOK_APPEND:
        return s_tok_append;
    case PARSE_TOK_IN:
        return s_tok_in;
    default:
        return s_tok_pipe;
    }
}

parse_status parse_line(char* line, parse_result* out)
{
    out->argc = 0;

    // Чтение и запись идут по одному буферу: запись никогда не обгоняет
    // чтение, потому что кавычки и экранирование только укорачивают текст.
    char* read = line;
    char* write = line;

    for (;;) {
        while (is_space(*read)) {
            ++read;
        }

        // Конец строки или комментарий: остаток не разбираем. `#` считается
        // комментарием только в начале слова - иначе `echo a#b` терял бы хвост.
        if (*read == '\0' || *read == '#') {
            break;
        }

        if (out->argc >= PARSE_MAX_ARGS) {
            return PARSE_ERR_ARGS;
        }

        // Служебный символ в начале слова: `echo a > f` и `echo a>f` дают одно
        // и то же - слово кончается там, где начинается символ.
        if (is_special(*read)) {
            int len = 0;
            const parse_token_kind kind = special_kind(*read, read, &len);
            read += len;
            out->argv[out->argc] = special_text(kind);
            out->kind[out->argc] = kind;
            ++out->argc;
            continue;
        }

        out->kind[out->argc] = PARSE_TOK_WORD;
        out->argv[out->argc++] = write;

        while (*read != '\0' && !is_space(*read) && !is_special(*read)) {
            const char c = *read;

            if (c == '\\') {
                ++read;
                if (*read == '\0') {
                    // Обратная косая черта в конце строки: экранировать нечего.
                    // Оставляем её буквально, а не выбрасываем молча.
                    *write++ = '\\';
                    break;
                }
                *write++ = *read++;
            } else if (c == '\'') {
                // Внутри одинарных кавычек буквально всё, включая `\`.
                ++read;
                while (*read != '\0' && *read != '\'') {
                    *write++ = *read++;
                }
                if (*read != '\'') {
                    return PARSE_ERR_QUOTE;
                }
                ++read;
            } else if (c == '"') {
                // В двойных кавычках `\` экранирует только кавычку и себя:
                // иначе `echo "C:\path"` терял бы буквы.
                ++read;
                while (*read != '\0' && *read != '"') {
                    if (*read == '\\' && (read[1] == '"' || read[1] == '\\')) {
                        ++read;
                    }
                    *write++ = *read++;
                }
                if (*read != '"') {
                    return PARSE_ERR_QUOTE;
                }
                ++read;
            } else {
                *write++ = *read++;
            }
        }

        // Что стоит за словом: пробел, конец строки или служебный символ.
        // Запомнить это надо до записи нуля - служебный байт сейчас будет
        // затёрт, а вид токена ещё понадобится.
        const char stop = *read;

        if (write == read) {
            // Слово не переписывалось: ноль ставим прямо на месте
            // разделителя. `read` при этом не сдвигаем - ниже он сдвинется
            // ровно на длину разделителя.
            if (stop != '\0') {
                *read = '\0';
                ++write;
            }
        } else {
            *write++ = '\0';
        }

        if (!is_special(stop)) {
            if (stop != '\0') {
                ++read;
            }
            continue;
        }

        // Служебный символ сразу за словом. Выпускаем его здесь же: байт в
        // строке уже затёрт нулём, и на следующем проходе цикла его бы не
        // стало.
        int len = 0;
        const parse_token_kind kind = special_kind(stop, read, &len);
        read += len;

        if (out->argc >= PARSE_MAX_ARGS) {
            return PARSE_ERR_ARGS;
        }
        out->argv[out->argc] = special_text(kind);
        out->kind[out->argc] = kind;
        ++out->argc;
    }

    return PARSE_OK;
}
