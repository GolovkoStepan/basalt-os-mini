// Разбор командной строки. Правила и почему так - см. parse.h.

#include "parse.h"

#include <stdbool.h>

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
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
        out->argv[out->argc++] = write;

        while (*read != '\0' && !is_space(*read)) {
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

        if (write == read) {
            // Слово без кавычек и экранирования: запись не сдвинулась, и
            // разделитель стоит ровно там, где нужен ноль. Затираем его на
            // месте - иначе следующий проход прочёл бы уже записанный ноль и
            // потерял бы остаток строки.
            if (*read != '\0') {
                *read++ = '\0';
            }
            write = read;
        } else {
            *write++ = '\0';
        }
    }

    return PARSE_OK;
}
