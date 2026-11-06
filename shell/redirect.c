// План выполнения командной строки. Правила и почему так - см. redirect.h.

#include "redirect.h"

#include <stdbool.h>
#include <stddef.h>

redirect_status redirect_build(parse_result* parsed, redirect_plan* out)
{
    out->count = 1;
    out->stage[0].argc = 0;
    out->stage[0].argv = &parsed->argv[0];
    out->stage[0].out_path = NULL;
    out->stage[0].append = false;
    out->stage[0].in_path = NULL;

    // Слова сдвигаются в начало того же массива: dst никогда не обгоняет src,
    // поэтому ещё не прочитанное не затирается. Тот же приём, что в разборе.
    int dst = 0;
    int start = 0;

    for (int i = 0; i < parsed->argc; ++i) {
        const parse_token_kind kind = parsed->kind[i];
        redirect_stage* const cur = &out->stage[out->count - 1];

        if (kind == PARSE_TOK_WORD) {
            parsed->argv[dst++] = parsed->argv[i];
            continue;
        }

        if (kind == PARSE_TOK_PIPE) {
            if (dst == start) {
                return REDIRECT_ERR_NO_COMMAND;
            }
            cur->argc = dst - start;

            if (out->count >= REDIRECT_MAX_STAGES) {
                return REDIRECT_ERR_STAGES;
            }

            start = dst;
            ++out->count;
            redirect_stage* const next = &out->stage[out->count - 1];
            next->argc = 0;
            next->argv = &parsed->argv[start];
            next->out_path = NULL;
            next->append = false;
            next->in_path = NULL;
            continue;
        }

        // `>`, `>>` и `<` требуют следующего слова: имя файла берётся из
        // строки, а не из пустоты.
        if (i + 1 >= parsed->argc || parsed->kind[i + 1] != PARSE_TOK_WORD) {
            return REDIRECT_ERR_NO_TARGET;
        }
        const char* const path = parsed->argv[i + 1];
        ++i;

        if (kind == PARSE_TOK_IN) {
            if (cur->in_path != NULL) {
                return REDIRECT_ERR_DOUBLE_IN;
            }
            cur->in_path = path;
        } else {
            if (cur->out_path != NULL) {
                return REDIRECT_ERR_DOUBLE_OUT;
            }
            cur->out_path = path;
            cur->append = (kind == PARSE_TOK_APPEND);
        }
    }

    if (dst == start) {
        return REDIRECT_ERR_NO_COMMAND;
    }
    out->stage[out->count - 1].argc = dst - start;

    return REDIRECT_OK;
}

const char* redirect_strerror(redirect_status st)
{
    switch (st) {
    case REDIRECT_OK:
        return "";
    case REDIRECT_ERR_NO_COMMAND:
        return "пустая команда";
    case REDIRECT_ERR_NO_TARGET:
        return "после >, >> или < нужно имя файла";
    case REDIRECT_ERR_DOUBLE_OUT:
        return "вывод перенаправлен дважды";
    case REDIRECT_ERR_DOUBLE_IN:
        return "ввод перенаправлен дважды";
    case REDIRECT_ERR_STAGES:
        return "слишком длинный конвейер";
    }
    return "неизвестная ошибка разбора перенаправлений";
}
