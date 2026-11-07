// Часы: точка отсчёта плюс прошедшее время. Устройство - в clock.h.

#include "clock.h"

void clock_reset(clock_t* c)
{
    if (c == NULL) {
        return;
    }
    c->base_unix = 0;
    c->base_mono = 0;
    c->synced = false;
}

void clock_set(clock_t* c, uint32_t unix_secs, uint64_t mono_us)
{
    if (c == NULL) {
        return;
    }
    c->base_unix = unix_secs;
    c->base_mono = mono_us;
    c->synced = true;
}

bool clock_synced(const clock_t* c)
{
    return c != NULL && c->synced;
}

bool clock_now(const clock_t* c, uint64_t mono_us, uint32_t* out)
{
    if (c == NULL || out == NULL || !c->synced) {
        return false;
    }
    // Монотонный счётчик за время работы не переполняется: 64 бита на 16 МГц
    // - это 36 000 лет. Поэтому вычитать можно без предосторожностей, но всё
    // равно проверяем «счётчик ушёл назад»: вызов с чужим счётчиком времени
    // (например, из теста или из другой точки старта) иначе дал бы дату в
    // прошлом без единого признака ошибки.
    if (mono_us < c->base_mono) {
        *out = c->base_unix;
        return true;
    }

    const uint64_t secs = (mono_us - c->base_mono) / 1000000u;
    const uint64_t total = (uint64_t)c->base_unix + secs;

    // Переполнение 32 бит - это 2106 год. Отдавать значение по кругу (1970-й)
    // нельзя: `ls -l` показал бы, что файл создан в прошлом веке.
    *out = (total > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)total;
    return true;
}

uint32_t clock_age_secs(const clock_t* c, uint64_t mono_us)
{
    if (c == NULL || !c->synced || mono_us < c->base_mono) {
        return 0;
    }
    const uint64_t secs = (mono_us - c->base_mono) / 1000000u;
    return (secs > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)secs;
}
