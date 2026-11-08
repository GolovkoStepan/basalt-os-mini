// Выбор задачи и пробуждение по сроку. Чистая логика, проверяется на маке.
//
// Здесь нет ни одного регистра и ни одного вызова планировщика: на вход идут
// только массивы состояний и сроков. Благодаря этому те же самые функции
// работают и на плате, и в тесте host - и тест проверяет именно тот код,
// который исполняется.

#include "sched.h"

size_t sched_pick(const basalt_task_state_t* states, size_t n, size_t from)
{
    if (states == NULL || n == 0) {
        return (size_t)-1;
    }

    // Обход начинается со следующего после `from`, а не с нуля: иначе задача с
    // наименьшим номером забирала бы процессор каждый раз, и до остальных
    // очередь не доходила бы никогда.
    for (size_t i = 1; i <= n; ++i) {
        const size_t k = (from + i) % n;
        const basalt_task_state_t st = states[k];
        if (st == BASALT_TASK_READY || st == BASALT_TASK_RUNNING) {
            return k;
        }
    }
    return (size_t)-1;
}

uint64_t sched_next_wake(const basalt_task_state_t* states, const uint64_t* wake_at, size_t n)
{
    if (states == NULL || wake_at == NULL) {
        return 0;
    }

    uint64_t best = 0;
    for (size_t i = 0; i < n; ++i) {
        if (states[i] != BASALT_TASK_BLOCKED) {
            continue;
        }
        // Срок 0 у спящей задачи - испорченное состояние: такую задачу не
        // разбудит никто, и она останется спящей навсегда. Считаем это «нет
        // срока», чтобы она хотя бы не влияла на решение «когда просыпаться».
        if (wake_at[i] == 0) {
            continue;
        }
        if (best == 0 || wake_at[i] < best) {
            best = wake_at[i];
        }
    }
    return best;
}

size_t sched_wake_due(basalt_task_state_t* states, const uint64_t* wake_at, size_t n, uint64_t now)
{
    if (states == NULL || wake_at == NULL) {
        return 0;
    }

    size_t woken = 0;
    for (size_t i = 0; i < n; ++i) {
        if (states[i] != BASALT_TASK_BLOCKED) {
            continue;
        }
        if (wake_at[i] == 0 || wake_at[i] > now) {
            continue;
        }
        states[i] = BASALT_TASK_READY;
        ++woken;
    }
    return woken;
}
