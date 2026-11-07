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
} task_t;

static task_t s_tasks[BASALT_TASK_MAX];
static size_t s_count;      // сколько слотов занято (включая DONE)
static size_t s_current;
static bool s_inited;
static uint32_t s_switches;

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
static size_t next_ready(size_t from)
{
    for (size_t i = 1; i <= s_count; ++i) {
        const size_t k = (from + i) % s_count;
        if (s_tasks[k].state == BASALT_TASK_READY || s_tasks[k].state == BASALT_TASK_RUNNING) {
            return k;
        }
    }
    return (size_t)-1;
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

    const size_t next = next_ready(s_current);
    if (next == (size_t)-1 || next == s_current) {
        return false;
    }
    switch_to(next);
    return true;
}

// Сколько задач готовы выполняться. Нужно basalt_task_exit: уходить с
// последней нельзя.
static size_t ready_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count; ++i) {
        if (s_tasks[i].state == BASALT_TASK_READY || s_tasks[i].state == BASALT_TASK_RUNNING) {
            ++n;
        }
    }
    return n;
}

void basalt_task_exit(void)
{
    s_tasks[s_current].state = BASALT_TASK_DONE;

    // Стек не освобождаем сразу: мы на нём стоим. Освободит тот, кто приберёт
    // слот, - в basalt_task_create. Освобождать из переключателя нельзя: там
    // не вызовешь ни одной функции C.
    if (ready_count() == 0) {
        // Стоять молча нельзя: плата просто замолчит, и это неотличимо от
        // зависшего USB-JTAG. Но и паника не годится - у неё нет кадра, и она
        // напечатала бы мусор вместо адреса. Оставляем системе шанс сказать
        // об этом самой: ждём прерываний, чтобы шелл мог напечатать строку.
        //
        // На практике сюда не попадают: главная задача не завершается.
        for (;;) {
            __asm__ volatile("waiti 2");
        }
    }

    const size_t next = next_ready(s_current);
    if (next == (size_t)-1) {
        for (;;) {
            __asm__ volatile("waiti 2");
        }
    }

    switch_to(next);
    // Сюда не возвращаемся: задачу больше не выберут.
    for (;;) {
    }
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
    return true;
}

size_t basalt_task_current(void)
{
    return s_current;
}
