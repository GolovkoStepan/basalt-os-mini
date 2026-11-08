// Задачи: таблица, стеки и выбор следующей. Переключение - в tasks.S.
//
// ПОЧЕМУ ПЕРЕКЛЮЧЕНИЕ В АССЕМБЛЕРЕ, А ТАБЛИЦА В C.
//
// Смена стека и сброс окон не терпят пролога C-функции: `entry` в начале уже
// выделил кадр на старом стеке, и после смены a1 этот кадр оказывается в
// чужой памяти. Поэтому сайт переключения - отдельная функция без пролога, а
// всё остальное (какие задачи есть, какая выбрана, сколько было переключений)
// живёт в C, где это проверяется на маке.
//
// ЧТО ИМЕННО СОХРАНЯЕТСЯ НА ЗАДАЧУ.
//
// Меньше, чем кажется. В оконном ABI a1 - это регистр своего окна: у каждой
// функции свой, и `retw` возвращает его вызывающему сам, из стека. Значит
// сохранять надо ровно два числа: указатель стека и PS. Адрес возврата уже
// лежит в кадре на этом стеке (его кладёт `entry`), а остальные регистры окна
// после SPILL_ALL_WINDOWS сброшены в кадры предков на тот же стек.
//
// Отсюда же ограничение: yield осмысленен только на границе вызовов, в
// верхнеуровневом цикле задачи. Локальные переменные, которые компилятор
// держал в регистрах вызывающего окна, после возврата восстанавливаются из
// стека - и это работает, - но состояние, живущее только в регистрах внутри
// длинного вычисления, переживёт переключение не обязано. У нас так и
// задумано: задача отдаёт процессор между итерациями, а не в середине счёта.

#include "tasks.h"

#include <string.h>

#include "esp_heap_caps.h"

#include "idle.h"
#include "sched.h"
#include "tick.h"

// Контекст переключения. Раскладка продублирована в tasks.S (CTX_*) и
// сверяется _Static_assert ниже: разъедутся - получим запись не туда, и это
// на плате выглядит как молчащая плата, а не как ошибка сборки.
typedef struct {
    uint32_t sp;    // 0: указатель стека задачи на точке переключения
    uint32_t ps;    // 4: PS на той же точке (уровень прерываний, CALLINC)
    uint32_t start; // 8: 1 - задача ещё не начиналась
    uint32_t fn;    // 12: что запускать при первом входе
    uint32_t arg;   // 16: аргумент
    uint32_t entry; // 20: адрес переходника (basalt_task_trampoline)
} switch_ctx_t;

_Static_assert(__builtin_offsetof(switch_ctx_t, sp) == 0, "ctx: sp");
_Static_assert(__builtin_offsetof(switch_ctx_t, ps) == 4, "ctx: ps");
_Static_assert(__builtin_offsetof(switch_ctx_t, start) == 8, "ctx: start");
_Static_assert(__builtin_offsetof(switch_ctx_t, fn) == 12, "ctx: fn");
_Static_assert(__builtin_offsetof(switch_ctx_t, arg) == 16, "ctx: arg");
_Static_assert(__builtin_offsetof(switch_ctx_t, entry) == 20, "ctx: entry");

typedef struct {
    switch_ctx_t ctx;
    const char* name;
    basalt_task_state_t state;
    uint8_t* stack;       // низ выделенного блока (его и освобождаем)
    size_t stack_size;
    uint32_t switches;
    uint64_t wake_at;     // спящая: uptime в мкс, когда проснуться
    uint64_t cpu_us;      // суммарное время на процессоре, мкс
    uint64_t cpu_us_at_sample;  // cpu_us на прошлом замере
    uint8_t cpu_percent;  // доля процессора с прошлого замера
} task_t;

static task_t s_tasks[BASALT_TASK_MAX];
static size_t s_count;      // сколько слотов занято (включая DONE)
static size_t s_current;
static bool s_inited;
static uint32_t s_switches;

// Учёт времени: сколько накопилось на каждой задаче и сколько система спала.
//
// Почему по времени, а не по числу переключений. Счётчик `switches` говорит,
// как часто задачу выбирали, но не сколько она работала: главная за одну
// итерацию читает USB, рисует экран и опрашивает кнопки, а демо-задача делает
// одну операцию - при равенстве переключений проценты вышли бы 50/50, и это
// выглядело бы как ошибка учёта. Время же есть время: при переключении
// накопленное отдаётся той задаче, которая только что работала.
static uint64_t s_now_us;    // момент последней смены задачи
static uint64_t s_idle_us;   // сколько система проспала в waiti
static uint64_t s_idle_us_at_sample;
static uint8_t s_idle_percent;  // доля сна за последний интервал

// Метка закраски стека задачи. Своя, а не из stack.c: у главной задачи стек
// закрашивает stack.c, и повторная закраска тем же значением не отличила бы
// «не трогали» от «уже писали».
#define TASK_PAINT 0xC3C3C3C3u

// PS для задач. НЕ константа, а снимок с текущего потока, и это осознанно.
//
// Соблазн выписать PS числом (у FreeRTOS это PS_UM|PS_EXCM|PS_WOE|PS_CALLINC(1))
// кончился бы тем, что задача пошла бы в другом режиме, чем остальная система.
// Замерено на плате: в нашем коде PS = 0x00060722 - это режим ядра (UM=0), а не
// пользовательский, как у FreeRTOS. Задача в режиме пользователя упала бы на
// первом же запрещённом обращении к регистру, и выглядело бы это как «задача
// не запускается».
//
// Поэтому берём PS у вызывающего и меняем ровно четыре вещи:
//   - INTLEVEL: ставим 2, чтобы тик (уровень 3) мог прервать задачу;
//   - EXCM: снимаем - в обычном коде он и так снят, а с ним `entry` в первой
//     же функции дал бы двойное исключение;
//   - CALLINC: обнуляем. Замерено на плате: PS вызывающего содержал
//     CALLINC=2, а `| 0x10000` давал 3, то есть call12. При CALLINC=3 пролог
//     раскладывает окно как после call12 и настраивает a13, а вызываемый по
//     call8 ищет указатель стека в a9. Задача получала в a1 мусор и падала
//     в слоте переполнения окна, а выглядело это как молчание после yield.
//     CALLINC=0 означает «обычный пролог» - ровно то, что нужно переходнику;
//   - OWB: обнуляем, потому что база окон - состояние потока, а не режим:
//     у вызывающего она своя, и новая задача начинает с начала файла окон.
uint32_t s_task_ps;

// Переключение. Единственная ассемблерная функция; см. tasks.S.
void basalt_task_switch(uint32_t* save_sp, switch_ctx_t* load);


// Переходник между переключателем и функцией задачи: зовёт её и закрывает
// задачу после возврата. Лежит в tasks.S рядом с переключателем, чтобы первый
// вход и обычное возобновление были в одном файле.
void basalt_task_trampoline(void);

static void stack_paint(uint8_t* bottom, size_t size)
{
    uint32_t* p = (uint32_t*)bottom;
    const size_t words = size / sizeof(uint32_t);
    for (size_t i = 0; i < words; ++i) {
        p[i] = TASK_PAINT;
    }
}

// Сколько стека занято: от дна до первого незакрашенного слова снизу. Тот же
// приём, что в stack.c, и по той же причине - нужен максимум за всё время, а
// не текущая глубина: текущая почти постоянна и о запасе ничего не говорит.
static size_t stack_used(const task_t* t)
{
    const uint32_t* p = (const uint32_t*)t->stack;
    const uint32_t* end = (const uint32_t*)(t->stack + t->stack_size);
    while (p < end && *p == TASK_PAINT) {
        ++p;
    }
    return (size_t)((const uint8_t*)end - (const uint8_t*)p);
}

void basalt_task_init(const char* name)
{
    if (s_inited) {
        return;
    }
    memset(s_tasks, 0, sizeof(s_tasks));
    s_count = 1;
    s_current = 0;
    s_switches = 0;

    s_tasks[0].name = name;
    s_tasks[0].state = BASALT_TASK_RUNNING;
    // Стек главной задачи нам не принадлежит: его задал start.S, и освобождать
    // его некому. stack = NULL - и create/exit по этому признаку не трогают
    // память.
    s_tasks[0].stack = NULL;
    s_tasks[0].stack_size = 0;

    // Точка отсчёта времени задач. Ноль означал бы, что первая же смена
    // приписала главной всё время с запуска счётчика (секунды паузы в старте).
    s_now_us = basalt_uptime_us();

    s_inited = true;

    // Снимок PS для новых задач: см. объяснение у s_task_ps. Берём именно
    // здесь, на своём стеке и в обычном режиме, а не в момент создания задачи:
    // между ними система могла оказаться в другом состоянии.
    uint32_t ps = 0;
    __asm__ volatile("rsr %0, ps" : "=r"(ps));
    // INTLEVEL=2 (тик уровня 3 должен прерывать), EXCM=0, CALLINC=0, OWB=0.
    // CALLINC и OWB именно снимаются: 0x1F покрывает INTLEVEL и EXCM, а
    // 0x30F00 - CALLINC (0x30000) и OWB (0xF00).
    s_task_ps = (ps & ~(uint32_t)0x00030F1Fu) | 2u;
}

bool basalt_task_create(const basalt_task_config_t* cfg)
{
    if (!s_inited || cfg == NULL || cfg->fn == NULL) {
        return false;
    }

    // Сначала ищем завершённую задачу: её стек мы можем освободить и занять
    // слот заново. Без этого система упёрлась бы в BASALT_TASK_MAX после
    // нескольких запусков, хотя живых задач одна.
    size_t slot = (size_t)-1;
    for (size_t i = 0; i < s_count; ++i) {
        if (s_tasks[i].state == BASALT_TASK_DONE) {
            slot = i;
            break;
        }
    }
    if (slot == (size_t)-1) {
        if (s_count >= BASALT_TASK_MAX) {
            return false;
        }
        slot = s_count;
        ++s_count;
    } else if (s_tasks[slot].stack != NULL) {
        // Стек завершённой задачи освобождаем здесь, а не в exit: в exit мы
        // на этом стеке стоим.
        heap_caps_free(s_tasks[slot].stack);
    }

    const size_t want = cfg->stack_size ? cfg->stack_size : BASALT_TASK_STACK_SIZE;

    // Только внутренняя память. Стек в PSRAM работал бы, но кэш драйвер flash
    // гасит на время записи, а задача, стоящая на стеке в PSRAM в этот момент,
    // просто перестанет существовать. Внутренней памяти на четыре стека хватает.
    uint8_t* stack = heap_caps_malloc(want, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stack == NULL) {
        return false;
    }
    stack_paint(stack, want);

    // Именно slot, а не s_count: при занятии завершённого слота s_count уже
    // больше slot, и запись в s_tasks[s_count] увела бы задачу в чужой пустой
    // слот, оставив занятый нулевым. На плате это выглядело как паника в
    // cmd_top: у задачи 1 оставался name == NULL, и strlen(NULL) давала
    // причину 28 с EXCVADDR 0.
    task_t* t = &s_tasks[slot];
    memset(t, 0, sizeof(*t));
    t->name = cfg->name ? cfg->name : "задача";
    t->state = BASALT_TASK_READY;
    t->stack = stack;
    t->stack_size = want;
    t->switches = 0;

    // Начальный стек. Вершина выравнивается на 16 - этого требует оконный ABI.
    //
    // Ниже вершины резервируются 64 байта, и в [sp-12] кладётся ссылка на
    // вершину. Это не запас «на всякий случай», а требование обработчика
    // переполнения окна: при callx8 аппаратура поворачивает окно ДО того, как
    // `entry` вызываемого успеет сохранить базу, и обработчик берёт указатель
    // стека предка из [sp-12]. Если там закраска, он пишет регистры по адресу
    // вида 0xC3C3C3A3 - на плате это выглядело как двойное исключение в слоте
    // переполнения окна. Ровно так готовит стек SET_STACK из
    // xt_instr_macros.h: sp = new_sp - SAVE_AREA_OFFSET, *(sp-12) = new_sp.
    uintptr_t top = (uintptr_t)(stack + want);
    top &= ~(uintptr_t)15u;
    const uintptr_t entry_sp = top - 64u;
    *(uint32_t*)(entry_sp - 12u) = (uint32_t)top;

    t->ctx.sp = (uint32_t)entry_sp;
    t->ctx.ps = s_task_ps;
    t->ctx.start = 1;
    t->ctx.fn = (uint32_t)(uintptr_t)cfg->fn;
    t->ctx.arg = (uint32_t)(uintptr_t)cfg->arg;
    t->ctx.entry = (uint32_t)(uintptr_t)basalt_task_trampoline;

    return true;
}

// Выбрать следующую готовую задачу, начиная с from+1, по кругу. Возвращает
// индекс или SIZE_MAX, если готовых больше нет.
//
// Сам обход живёт в sched.c: это чистая логика, и на маке её проверяет
// test_sched. Здесь только сборка состояний в массив - раскладка task_t нам
// принадлежит, а sched.c про неё знать не должен.
static size_t state_snapshot(basalt_task_state_t* out)
{
    for (size_t i = 0; i < s_count; ++i) {
        out[i] = s_tasks[i].state;
    }
    return s_count;
}

static size_t next_ready(size_t from)
{
    basalt_task_state_t states[BASALT_TASK_MAX];
    const size_t n = state_snapshot(states);
    return sched_pick(states, n, from);
}

static void switch_to(size_t next)
{
    task_t* cur = &s_tasks[s_current];
    task_t* nxt = &s_tasks[next];

    // Завершённую задачу обратно в «готова» не переводим: иначе basalt_task_exit
    // воскрешал бы задачу, которая уже вернулась из своей функции, и yield
    // выбирал бы её снова - с кадром, которого больше нет.
    if (cur->state == BASALT_TASK_RUNNING) {
        cur->state = BASALT_TASK_READY;
    }
    nxt->state = BASALT_TASK_RUNNING;
    nxt->switches += 1;
    s_switches += 1;

    // Время на процессоре отдаём тому, кто работал до переключения. Читаем
    // uptime здесь, а не после - иначе в `cur` записалась бы и наша собственная
    // работа по переключению.
    const uint64_t now = basalt_uptime_us();
    cur->cpu_us += now - s_now_us;
    s_now_us = now;

    const size_t prev = s_current;
    s_current = next;

    // Вызов переключения с последней строкой «вернулись сюда». После
    // возобновления этой задачи управление придёт ровно в эту точку - так
    // устроен retw в tasks.S, - и дальше обычный возврат в yield.
    basalt_task_switch(&s_tasks[prev].ctx.sp, &nxt->ctx);
}

bool basalt_task_yield(void)
{
    if (!s_inited || s_count < 2) {
        // Переключаться не на кого. Это не ошибка: main-цикл зовёт yield
        // постоянно, и в системе из одной задачи он обязан быть безвредным.
        return false;
    }

    // Пробуждение по сроку - здесь, а не в главном цикле и не в обработчике
    // тика. Выбор задачи и есть тот момент, когда «кто-то готов» пересчитывается
    // заново; в главном цикле это было бы привязано к одному вызывающему
    // (задача, которая спит внутри basalt_task_sleep_ms, его не проходит), а в
    // обработчике тика - потребовало бы вызова C-функции из прерывания, чего
    // обработчик не делает.
    //
    // Цена: обход таблицы (не больше BASALT_TASK_MAX) на каждое переключение.
    // Замерено около 150 тысяч переключений в секунду, то есть сотни тысяч
    // сравнений - на 160 МГц это ничто.
    (void)basalt_task_wake_due();

    const size_t next = next_ready(s_current);
    if (next == (size_t)-1 || next == s_current) {
        return false;
    }
    switch_to(next);
    return true;
}

// Сколько задач готовы выполняться. Ноль значит «все спят»: только тогда
// уместен сон системы, иначе готовую задачу никто не выберет.
size_t basalt_task_ready_count(void)
{
    basalt_task_state_t states[BASALT_TASK_MAX];
    const size_t n = state_snapshot(states);
    size_t ready = 0;
    for (size_t i = 0; i < n; ++i) {
        if (states[i] == BASALT_TASK_READY || states[i] == BASALT_TASK_RUNNING) {
            ++ready;
        }
    }
    return ready;
}

void basalt_task_sleep_ms(uint32_t ms)
{
    if (!s_inited) {
        // Планировщика нет: единственный способ подождать - занять ядро.
        // Так бывает только в env'ах без shell, и лучше явная задержка, чем
        // молчаливое «сон не работает».
        basalt_idle_ms(ms);
        return;
    }

    if (ms == 0) {
        // «До следующего тика»: уступаем, если есть кому, и спим, если нет.
        if (!basalt_task_yield()) {
            basalt_task_wait();
        }
        return;
    }

    s_tasks[s_current].wake_at = basalt_uptime_us() + (uint64_t)ms * 1000u;

    // Состояние меняется ДО переключения, иначе задача снова попала бы в выбор
    // на следующем же yield: switch_to переводит в READY только RUNNING.
    s_tasks[s_current].state = BASALT_TASK_BLOCKED;

    // Ждём возврата. Пробуждение делает basalt_task_yield: он выбирает
    // следующую задачу, а значит именно он пересчитывает «кто готов». Если
    // готовых нет, спим всей системой - иначе ожидание превратилось бы в
    // холостой цикл, в котором некому разбудить даже себя.
    for (;;) {
        // Состояние проверяем до уступки: срок мог наступить, пока мы тут
        // стояли. После уступки проверять не нужно - yield уже разбудил
        // просроченных, и если нас выбрали, мы больше не BLOCKED.
        if (s_tasks[s_current].state != BASALT_TASK_BLOCKED) {
            return;
        }
        (void)basalt_task_wake_due();
        if (s_tasks[s_current].state != BASALT_TASK_BLOCKED) {
            return;
        }
        if (!basalt_task_yield()) {
            basalt_task_wait();
        }
    }
}

void basalt_task_wait(void)
{
    if (!s_inited) {
        basalt_idle_wait();
        return;
    }

    // Разбудить тех, чей срок наступил, и только потом спать. Порядок важен:
    // если спать первым, разбудившее нас прерывание вернёт управление уже
    // после проверки, и просроченная задача простояла бы лишний тик.
    (void)basalt_task_wake_due();

    // Сон системы тоже идёт в счёт интервала: без него `top` показывал бы
    // 100 % занятости у задач, даже когда система почти всё время спит.
    const uint64_t before = basalt_uptime_us();

    // `waiti` возвращается на любом прерывании, а тик идёт 100 раз в секунду,
    // поэтому условие перепроверяется вызывающим, а не здесь. Спать «ровно до
    // срока» бессмысленно: ближайший тик всё равно наступит раньше 10 мс.
    basalt_idle_wait();

    const uint64_t after = basalt_uptime_us();
    s_idle_us += after - before;

    // Время сна не принадлежит ни одной задаче: сдвигаем точку отсчёта, иначе
    // следующая смена приписала бы его той задаче, что работала до сна.
    s_now_us = after;
}

void basalt_task_exit(void)
{
    task_t* cur = &s_tasks[s_current];
    cur->state = BASALT_TASK_DONE;

    // Сон задачи, которая завершилась, больше не срок: иначе пробуждение
    // перевело бы DONE в READY, и yield выбрал бы задачу, чей кадр на стеке
    // уже мёртв.
    cur->wake_at = 0;

    // Стек не освобождаем сразу: мы на нём стоим. Освободит тот, кто приберёт
    // слот, - в basalt_task_create. Освобождать из переключателя нельзя: там
    // не вызовешь ни одной функции C.
    //
    // Выбирать следующего приходится в цикле, а не одним заходом. С шага 3
    // задача может СПАТЬ, и «готовых нет» больше не значит «в системе больше
    // никого»: остальные спят и ждут срока. Если просто уйти в waiti, разбудить
    // их будет некому - wake_due зовут выполняющиеся задачи, а выполняющихся
    // не осталось. На плате это выглядело бы как молчащая плата ровно после
    // того, как задача с `sleep` вернулась.
    //
    // Стоять молча тоже нельзя: плата замолчит, и это неотличимо от зависшего
    // USB-JTAG. Но и паника не годится - у неё нет кадра, и она напечатала бы
    // мусор вместо адреса.
    size_t next = next_ready(s_current);
    while (next == (size_t)-1) {
        // basalt_task_wait, а не ручной wake_due + idle_wait: он же учитывает
        // время сна в статистике. Иначе простой здесь записался бы в cpu_us
        // завершившейся задачи и всплыл бы в `top` у того, кого уже нет.
        basalt_task_wait();
        next = next_ready(s_current);
    }

    switch_to(next);
    // Сюда не возвращаемся: задачу больше не выберут.
    for (;;) {
    }
}

uint64_t basalt_task_next_wake(void)
{
    // Сроки лежат в структуре задачи, а sched.c про неё не знает: поэтому
    // собираем два параллельных массива. Размер - по константе, а не по
    // s_count: массив на стеке, и лишние слоты ничего не стоят.
    basalt_task_state_t states[BASALT_TASK_MAX];
    uint64_t wake_at[BASALT_TASK_MAX];
    for (size_t i = 0; i < s_count; ++i) {
        states[i] = s_tasks[i].state;
        wake_at[i] = s_tasks[i].wake_at;
    }
    return sched_next_wake(states, wake_at, s_count);
}

size_t basalt_task_wake_due(void)
{
    basalt_task_state_t states[BASALT_TASK_MAX];
    uint64_t wake_at[BASALT_TASK_MAX];
    for (size_t i = 0; i < s_count; ++i) {
        states[i] = s_tasks[i].state;
        wake_at[i] = s_tasks[i].wake_at;
    }

    const size_t woken = sched_wake_due(states, wake_at, s_count, basalt_uptime_us());
    if (woken == 0) {
        return 0;
    }

    // Обратная запись: какие именно задачи разбудили, sched_wake_due не
    // сообщает - ей достаточно числа. Сверяем по изменившемуся состоянию, а
    // не по сроку: срок мог остаться тем же, а решение «проснулась» - уже
    // принятым.
    for (size_t i = 0; i < s_count; ++i) {
        if (s_tasks[i].state == BASALT_TASK_BLOCKED && states[i] == BASALT_TASK_READY) {
            s_tasks[i].state = BASALT_TASK_READY;
            s_tasks[i].wake_at = 0;
        }
    }
    return woken;
}

void basalt_task_cpu_sample(void)
{
    // Сначала закрываем текущий интервал: время с прошлой смены принадлежит
    // той задаче, которая работает прямо сейчас. Без этого она потеряла бы
    // последний кусок, и сумма долей не сходилась бы с интервалом.
    const uint64_t now = basalt_uptime_us();
    s_tasks[s_current].cpu_us += now - s_now_us;
    s_now_us = now;

    // Разницы считаем в массив ДО обновления снимка. Если обновить счётчики в
    // первом же проходе и считать доли во втором, второй проход увидит нули, и
    // проценты всегда были бы нулевыми - ошибка тихая, `top` просто показывал
    // бы 0 % у всех.
    uint64_t delta[BASALT_TASK_MAX];
    uint64_t total = 0;
    for (size_t i = 0; i < s_count; ++i) {
        delta[i] = s_tasks[i].cpu_us - s_tasks[i].cpu_us_at_sample;
        s_tasks[i].cpu_us_at_sample = s_tasks[i].cpu_us;
        total += delta[i];
    }

    // Сон системы - тоже часть интервала, и в долях задач он не участвует.
    // Без него сумма процентов всегда была бы 100, и было бы не видно, что
    // система в простое почти всё время спит.
    const uint64_t idle = s_idle_us - s_idle_us_at_sample;
    s_idle_us_at_sample = s_idle_us;
    total += idle;

    if (total == 0) {
        // Делить не на что. Проценты прошлого интервала не обнуляем: ноль
        // выглядел бы как «всё встало», хотя это значило бы «интервал пуст».
        return;
    }

    // Доли считаются с округлением вниз: остаток уходит сну, потому что он
    // появился именно из-за waiti. Поэтому сумма «задачи + сон» на экране
    // всегда ровно 100, и разбираться с недобором не приходится.
    uint32_t given = 0;

    for (size_t i = 0; i < s_count; ++i) {
        s_tasks[i].cpu_percent = 0;
    }

    for (size_t i = 0; i < s_count; ++i) {
        if (delta[i] == 0) {
            continue;
        }
        const uint32_t share = (uint32_t)((delta[i] * 100u) / total);
        s_tasks[i].cpu_percent = (uint8_t)(share > 100u ? 100u : share);
        given += s_tasks[i].cpu_percent;
    }

    s_idle_percent = (uint8_t)(given < 100u ? 100u - given : 0u);
}

uint8_t basalt_task_idle_percent(void)
{
    return s_idle_percent;
}

uint32_t basalt_task_switches(void)
{
    return s_switches;
}

size_t basalt_task_count(void)
{
    return s_count;
}

bool basalt_task_get(size_t index, basalt_task_info_t* out)
{
    if (index >= s_count || out == NULL) {
        return false;
    }
    const task_t* t = &s_tasks[index];
    out->name = t->name;
    out->state = t->state;
    out->stack_size = t->stack_size;
    // У главной задачи размер стека знает stack.c, а указателя на дно у
    // планировщика нет. Отчёт для неё заполняет вызывающий - см. cmd_top.
    out->stack_used = t->stack ? stack_used(t) : 0;
    out->switches = t->switches;
    out->cpu_percent = t->cpu_percent;

    // Сколько ещё ждать. Считаем от текущего времени, а не храним: остаток
    // меняется сам, и запоминать его пришлось бы обновлять на каждом тике.
    out->wake_in_ms = 0;
    if (t->state == BASALT_TASK_BLOCKED && t->wake_at != 0) {
        const uint64_t now = basalt_uptime_us();
        if (t->wake_at > now) {
            out->wake_in_ms = (uint32_t)((t->wake_at - now + 999u) / 1000u);
        }
    }
    return true;
}

size_t basalt_task_current(void)
{
    return s_current;
}
