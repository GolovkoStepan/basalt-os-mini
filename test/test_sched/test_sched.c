// Тесты выбора задачи и пробуждения по сроку на маке.
//
// Почему здесь, а не на плате: ошибки в обходе круга и в сравнении сроков
// тихие. Задача, выбранная дважды подряд, выглядит как «одна работает
// медленнее», а задача, не выбранная ни разу, - как «зависла». На маке эти
// случаи перебираются целиком, включая границу «срок наступил ровно сейчас».

#include <unity.h>

#include <string.h>

#include "sched.h"

void setUp(void)
{
}

void tearDown(void)
{
}

// ---------------------------------------------------------------------------
// sched_pick: обход круга
// ---------------------------------------------------------------------------

static void test_pick_goes_round_in_order(void)
{
    const basalt_task_state_t st[] = {BASALT_TASK_RUNNING, BASALT_TASK_READY, BASALT_TASK_READY};

    TEST_ASSERT_EQUAL_size_t(1, sched_pick(st, 3, 0));
    TEST_ASSERT_EQUAL_size_t(2, sched_pick(st, 3, 1));
    // С третьей - обратно на первую, а не в «нет готовых».
    TEST_ASSERT_EQUAL_size_t(0, sched_pick(st, 3, 2));
}

static void test_pick_skips_blocked_and_done(void)
{
    const basalt_task_state_t st[] = {BASALT_TASK_RUNNING, BASALT_TASK_BLOCKED, BASALT_TASK_DONE,
                                      BASALT_TASK_READY};

    TEST_ASSERT_EQUAL_size_t(3, sched_pick(st, 4, 0));
    TEST_ASSERT_EQUAL_size_t(0, sched_pick(st, 4, 3));
}

static void test_pick_none_ready(void)
{
    const basalt_task_state_t st[] = {BASALT_TASK_BLOCKED, BASALT_TASK_DONE, BASALT_TASK_FREE};

    TEST_ASSERT_EQUAL_size_t((size_t)-1, sched_pick(st, 3, 0));
}

static void test_pick_single_task_returns_itself(void)
{
    // В системе из одной задачи ответ - её же индекс. Главный цикл по этому
    // признаку решает, что переключаться не на кого.
    const basalt_task_state_t st[] = {BASALT_TASK_RUNNING};

    TEST_ASSERT_EQUAL_size_t(0, sched_pick(st, 1, 0));
    TEST_ASSERT_EQUAL_size_t(0, sched_pick(st, 1, 5));
}

static void test_pick_from_out_of_range(void)
{
    const basalt_task_state_t st[] = {BASALT_TASK_READY, BASALT_TASK_BLOCKED};

    // from вне диапазона - не ошибка: обход начинается со следующего номера по
    // модулю. Проверяем, что функция не читает за границей массива.
    TEST_ASSERT_EQUAL_size_t(0, sched_pick(st, 2, 100));
    TEST_ASSERT_EQUAL_size_t(0, sched_pick(st, 2, (size_t)-1));
}

static void test_pick_empty_and_null(void)
{
    const basalt_task_state_t st[] = {BASALT_TASK_READY};

    TEST_ASSERT_EQUAL_size_t((size_t)-1, sched_pick(st, 0, 0));
    TEST_ASSERT_EQUAL_size_t((size_t)-1, sched_pick(NULL, 3, 0));
}

// ---------------------------------------------------------------------------
// sched_next_wake: ближайший срок
// ---------------------------------------------------------------------------

static void test_next_wake_none(void)
{
    const basalt_task_state_t st[] = {BASALT_TASK_RUNNING, BASALT_TASK_READY};

    TEST_ASSERT_EQUAL_UINT64(0, sched_next_wake(st, (const uint64_t[]){0, 0}, 2));
}

static void test_next_wake_earliest(void)
{
    const basalt_task_state_t st[] = {BASALT_TASK_READY, BASALT_TASK_BLOCKED, BASALT_TASK_BLOCKED};
    const uint64_t wake[] = {0, 500000, 200000};

    TEST_ASSERT_EQUAL_UINT64(200000, sched_next_wake(st, wake, 3));
}

static void test_next_wake_ignores_zero_of_sleeper(void)
{
    // Спящая задача с нулевым сроком - испорченное состояние. Она не должна
    // давать «ближайший срок 0»: иначе система решила бы, что просыпаться
    // некому, и проспала бы чужой настоящий срок.
    const basalt_task_state_t st[] = {BASALT_TASK_BLOCKED, BASALT_TASK_BLOCKED};
    const uint64_t wake[] = {0, 700000};

    TEST_ASSERT_EQUAL_UINT64(700000, sched_next_wake(st, wake, 2));
}

static void test_next_wake_ignores_awake(void)
{
    // Срок у готовой задачи - мусор: проснулась, значит срок больше не нужен.
    const basalt_task_state_t st[] = {BASALT_TASK_READY, BASALT_TASK_BLOCKED};
    const uint64_t wake[] = {1000, 900000};

    TEST_ASSERT_EQUAL_UINT64(900000, sched_next_wake(st, wake, 2));
}

// ---------------------------------------------------------------------------
// sched_wake_due: граница срока
// ---------------------------------------------------------------------------

static void test_wake_due_exact_boundary(void)
{
    // Срок ровно `now` уже наступил. Строгое `<` оставило бы задачу спать до
    // следующего тика без причины - это и есть тот случай, который проверяем.
    basalt_task_state_t st[] = {BASALT_TASK_BLOCKED};
    const uint64_t wake[] = {1000};

    TEST_ASSERT_EQUAL_size_t(1, sched_wake_due(st, wake, 1, 1000));
    TEST_ASSERT_EQUAL(BASALT_TASK_READY, st[0]);
}

static void test_wake_due_not_yet(void)
{
    basalt_task_state_t st[] = {BASALT_TASK_BLOCKED};
    const uint64_t wake[] = {1001};

    TEST_ASSERT_EQUAL_size_t(0, sched_wake_due(st, wake, 1, 1000));
    TEST_ASSERT_EQUAL(BASALT_TASK_BLOCKED, st[0]);
}

static void test_wake_due_skips_non_blocked(void)
{
    // Готовая задача с прошедшим сроком остаётся готовой: повторное
    // пробуждение посчитало бы её дважды и сбило бы счётчик woken.
    basalt_task_state_t st[] = {BASALT_TASK_READY, BASALT_TASK_BLOCKED, BASALT_TASK_DONE};
    const uint64_t wake[] = {1, 2, 3};

    TEST_ASSERT_EQUAL_size_t(1, sched_wake_due(st, wake, 3, 100));
    TEST_ASSERT_EQUAL(BASALT_TASK_READY, st[0]);
    TEST_ASSERT_EQUAL(BASALT_TASK_READY, st[1]);
    TEST_ASSERT_EQUAL(BASALT_TASK_DONE, st[2]);
}

static void test_wake_due_several(void)
{
    basalt_task_state_t st[] = {BASALT_TASK_BLOCKED, BASALT_TASK_BLOCKED, BASALT_TASK_BLOCKED};
    const uint64_t wake[] = {100, 200, 300};

    TEST_ASSERT_EQUAL_size_t(2, sched_wake_due(st, wake, 3, 250));
    TEST_ASSERT_EQUAL(BASALT_TASK_READY, st[0]);
    TEST_ASSERT_EQUAL(BASALT_TASK_READY, st[1]);
    TEST_ASSERT_EQUAL(BASALT_TASK_BLOCKED, st[2]);
}

static void test_wake_due_ignores_zero_sleeper(void)
{
    // Нулевой срок не будим: задача без срока проснулась бы немедленно и
    // оказалась бы готовой навсегда, съедая процессор в круге.
    basalt_task_state_t st[] = {BASALT_TASK_BLOCKED};
    const uint64_t wake[] = {0};

    TEST_ASSERT_EQUAL_size_t(0, sched_wake_due(st, wake, 1, 1000));
    TEST_ASSERT_EQUAL(BASALT_TASK_BLOCKED, st[0]);
}

static void test_wake_due_empty(void)
{
    basalt_task_state_t st[] = {BASALT_TASK_READY};

    TEST_ASSERT_EQUAL_size_t(0, sched_wake_due(st, (const uint64_t[]){0}, 0, 1000));
    TEST_ASSERT_EQUAL_size_t(0, sched_wake_due(NULL, NULL, 1, 1000));
}

// ---------------------------------------------------------------------------
// Сценарий: спящая задача не мешает остальным
// ---------------------------------------------------------------------------

static void test_scenario_one_sleeps_others_run(void)
{
    // Три задачи: главная работает, вторая спит, третья готова. Сон второй не
    // должен ни выбираться в круг, ни мешать остальным.
    basalt_task_state_t st[] = {BASALT_TASK_RUNNING, BASALT_TASK_BLOCKED, BASALT_TASK_READY};
    uint64_t wake[] = {0, 5000, 0};

    TEST_ASSERT_EQUAL_size_t(2, sched_pick(st, 3, 0));
    TEST_ASSERT_EQUAL_UINT64(5000, sched_next_wake(st, wake, 3));

    // Срок ещё не пришёл - вторая по-прежнему спит.
    TEST_ASSERT_EQUAL_size_t(0, sched_wake_due(st, wake, 3, 4000));
    TEST_ASSERT_EQUAL(BASALT_TASK_BLOCKED, st[1]);

    // Пришёл - просыпается и попадает в круг.
    TEST_ASSERT_EQUAL_size_t(1, sched_wake_due(st, wake, 3, 5000));
    TEST_ASSERT_EQUAL(BASALT_TASK_READY, st[1]);
    TEST_ASSERT_EQUAL_size_t(1, sched_pick(st, 3, 0));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_pick_goes_round_in_order);
    RUN_TEST(test_pick_skips_blocked_and_done);
    RUN_TEST(test_pick_none_ready);
    RUN_TEST(test_pick_single_task_returns_itself);
    RUN_TEST(test_pick_from_out_of_range);
    RUN_TEST(test_pick_empty_and_null);

    RUN_TEST(test_next_wake_none);
    RUN_TEST(test_next_wake_earliest);
    RUN_TEST(test_next_wake_ignores_zero_of_sleeper);
    RUN_TEST(test_next_wake_ignores_awake);

    RUN_TEST(test_wake_due_exact_boundary);
    RUN_TEST(test_wake_due_not_yet);
    RUN_TEST(test_wake_due_skips_non_blocked);
    RUN_TEST(test_wake_due_several);
    RUN_TEST(test_wake_due_ignores_zero_sleeper);
    RUN_TEST(test_wake_due_empty);

    RUN_TEST(test_scenario_one_sleeps_others_run);

    return UNITY_END();
}
