// История команд. Зачем отдельный модуль - см. history.h.

#include "history.h"

#include <string.h>

static char s_lines[HISTORY_CAP][HISTORY_LINE_CAP];

// Куда писать следующую команду. Кольцо: когда места нет, самая старая запись
// затирается следующей же вставкой, и отдельного сдвига не нужно.
static int s_head;
static int s_count;

void history_init(void)
{
    memset(s_lines, 0, sizeof(s_lines));
    s_head = 0;
    s_count = 0;
}

void history_clear(void)
{
    history_init();
}

int history_count(void)
{
    return s_count;
}

// Самая старая из хранимых записей. При неполном кольце это нулевой элемент,
// при полном - тот, что вот-вот затрётся.
static int oldest(void)
{
    int idx = s_head - s_count;
    while (idx < 0) {
        idx += HISTORY_CAP;
    }
    return idx % HISTORY_CAP;
}

const char* history_get(int index)
{
    if (index < 0 || index >= s_count) {
        return "";
    }

    const int idx = (oldest() + index) % HISTORY_CAP;
    return s_lines[idx];
}

bool history_push(const char* line)
{
    if (line == NULL || line[0] == '\0') {
        return false;
    }

    // Повтор последней не храним: чаще всего это Enter по уже набранной
    // команде, и без проверки история состояла бы из неё одной.
    if (s_count > 0 && strcmp(history_get(s_count - 1), line) == 0) {
        return false;
    }

    const size_t len = strlen(line);
    if (len >= HISTORY_LINE_CAP) {
        return false;
    }

    memcpy(s_lines[s_head], line, len + 1);
    s_lines[s_head][len] = '\0';

    s_head = (s_head + 1) % HISTORY_CAP;
    if (s_count < HISTORY_CAP) {
        ++s_count;
    }
    return true;
}
