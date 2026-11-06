// Тесты декодера UTF-8 на хосте.
//
// Почему эти случаи: кириллица приходит из USB двумя байтами, и граница
// символа может попасть на границу двух вызовов text_putc. Ошибка здесь
// выглядит на экране как «буква не та» через несколько символов после сбоя -
// искать её на плате почти невозможно.
//
// Отдельно проверяются отказы: overlong-запись, суррогаты, значения больше
// 0x10FFFF. Всё это формально можно закодировать, и всё это не символы.

#include <unity.h>

#include <stdint.h>

#include "utf8.h"

static utf8_decoder s_dec;
static uint32_t s_out;

void setUp(void)
{
    utf8_reset(&s_dec);
    s_out = 0xFFFFFFFFu;
}

void tearDown(void)
{
}

// Подать строку целиком; вернуть код последнего собранного символа.
// -1, если последовательность отвергнута.
static int feed(const char* s)
{
    int result = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p != 0; ++p) {
        const utf8_status st = utf8_feed(&s_dec, *p, &s_out);
        if (st == UTF8_INVALID) {
            return -1;
        }
        result = (st == UTF8_DONE) ? (int)s_out : result;
    }
    return result;
}

// --- ASCII ---

static void test_ascii_passes_through(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_DONE, utf8_feed(&s_dec, 'A', &s_out));
    TEST_ASSERT_EQUAL_HEX32('A', s_out);

    TEST_ASSERT_EQUAL_INT(UTF8_DONE, utf8_feed(&s_dec, 0x00, &s_out));
    TEST_ASSERT_EQUAL_HEX32(0x00, s_out);
}

// --- кириллица ---

// «А» - 0xD0 0x90. Первый байт только копит биты, символ появляется на втором.
static void test_cyrillic_a_two_bytes(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_PENDING, utf8_feed(&s_dec, 0xD0, &s_out));
    TEST_ASSERT_EQUAL_INT(UTF8_DONE, utf8_feed(&s_dec, 0x90, &s_out));
    TEST_ASSERT_EQUAL_HEX32(0x0410, s_out);
}

static void test_cyrillic_lowercase_ya(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x044F, (uint32_t)feed("\xD1\x8F"));
}

// Ё и ё стоят вне блока 0x410..0x44F: их коды 0x401 и 0x451.
static void test_cyrillic_yo(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x0401, (uint32_t)feed("\xD0\x81"));
    utf8_reset(&s_dec);
    TEST_ASSERT_EQUAL_HEX32(0x0451, (uint32_t)feed("\xD1\x91"));
}

// Строка целиком: «Привет» - шесть символов, каждый по два байта.
static void test_cyrillic_word_stream(void)
{
    static const char kWord[] = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";

    int count = 0;
    for (const unsigned char* p = (const unsigned char*)kWord; *p != 0; ++p) {
        const utf8_status st = utf8_feed(&s_dec, *p, &s_out);
        if (st == UTF8_DONE) {
            ++count;
        }
        TEST_ASSERT_NOT_EQUAL(UTF8_INVALID, st);
    }
    TEST_ASSERT_EQUAL_INT(6, count);
    TEST_ASSERT_EQUAL_HEX32(0x0442, s_out);  // последний - «т»
}

// --- трёх- и четырёхбайтные ---

static void test_three_byte(void)
{
    // U+20AC, знак евро.
    TEST_ASSERT_EQUAL_HEX32(0x20AC, (uint32_t)feed("\xE2\x82\xAC"));
}

static void test_four_byte(void)
{
    // U+1F600, эмодзи. Глифа нет, но декодер обязан собрать код.
    TEST_ASSERT_EQUAL_HEX32(0x1F600, (uint32_t)feed("\xF0\x9F\x98\x80"));
}

// --- отказы ---

// Продолжение без головки.
static void test_orphan_continuation(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0x80, &s_out));
}

// 0xC0/0xC1 бывают только у overlong-записей: настоящих символов там нет.
static void test_c0_c1_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0xC0, &s_out));
    utf8_reset(&s_dec);
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0xC1, &s_out));
}

// 0xF5..0xFF вышли бы за 0x10FFFF.
static void test_f5_and_ff_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0xF5, &s_out));
    utf8_reset(&s_dec);
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0xFF, &s_out));
}

// Overlong-запись NUL: 0xC0 0x80. Ловушка классическая - такой байт прошёл бы
// мимо любого фильтра, который ищет 0x00. Головка 0xC0 отвергается сразу,
// поэтому 0x80 следом - уже продолжение без головки.
static void test_overlong_nul_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0xC0, &s_out));
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0x80, &s_out));
}

// Overlong 0xE0 0x80 0xAF вместо '/' (0x2F).
static void test_overlong_slash_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_PENDING, utf8_feed(&s_dec, 0xE0, &s_out));
    TEST_ASSERT_EQUAL_INT(UTF8_PENDING, utf8_feed(&s_dec, 0x80, &s_out));
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0xAF, &s_out));
}

// Суррогаты 0xD800..0xDFFF в UTF-8 недопустимы: так кодировали в UTF-16.
static void test_surrogate_rejected(void)
{
    // U+D800 = 0xED 0xA0 0x80.
    TEST_ASSERT_EQUAL_INT(UTF8_PENDING, utf8_feed(&s_dec, 0xED, &s_out));
    TEST_ASSERT_EQUAL_INT(UTF8_PENDING, utf8_feed(&s_dec, 0xA0, &s_out));
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0x80, &s_out));
}

// Битый хвост: головка ждёт продолжение, а пришёл ASCII. Символ не собран,
// состояние сброшено - следующий байт разбирается как новый символ.
static void test_broken_tail_recovers(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_PENDING, utf8_feed(&s_dec, 0xD0, &s_out));
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 'x', &s_out));

    TEST_ASSERT_EQUAL_INT(UTF8_DONE, utf8_feed(&s_dec, 'y', &s_out));
    TEST_ASSERT_EQUAL_HEX32('y', s_out);
}

// Обрыв на границе вызова: состояние сохраняется между байтами.
static void test_state_survives_calls(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_PENDING, utf8_feed(&s_dec, 0xD0, &s_out));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, s_out);  // ничего не записали

    TEST_ASSERT_EQUAL_INT(UTF8_DONE, utf8_feed(&s_dec, 0x96, &s_out));
    TEST_ASSERT_EQUAL_HEX32(0x0416, s_out);  // «Ж»
}

// Сброс посреди последовательности: недособранная буква не должна склеиться
// с хвостом, который придёт после сброса.
static void test_reset_drops_pending(void)
{
    TEST_ASSERT_EQUAL_INT(UTF8_PENDING, utf8_feed(&s_dec, 0xD0, &s_out));
    utf8_reset(&s_dec);
    TEST_ASSERT_EQUAL_INT(UTF8_INVALID, utf8_feed(&s_dec, 0x90, &s_out));
}

// --- кодировщик ---

// Простой случай: ASCII кодируется одним байтом, и это тот же байт.
static void test_encode_ascii_one_byte(void)
{
    char out[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(1, (int)utf8_encode('A', out));
    TEST_ASSERT_EQUAL_HEX8('A', (uint8_t)out[0]);
}

// Кириллица - два байта. Сверяем с тем, что приходит из терминала: если
// кодировщик и декодер разойдутся, эхо покажет не ту букву, а искать причину
// будут в драйвере USB.
static void test_encode_cyrillic_two_bytes(void)
{
    char out[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(2, (int)utf8_encode(0x0416, out));  // «Ж»
    TEST_ASSERT_EQUAL_HEX8(0xD0, (uint8_t)out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x96, (uint8_t)out[1]);
}

// Границы разрядности: 0x7F/0x80 и 0x7FF/0x800 - там меняется длина записи.
static void test_encode_length_boundaries(void)
{
    char out[4];

    TEST_ASSERT_EQUAL_INT(1, (int)utf8_encode(0x7F, out));
    TEST_ASSERT_EQUAL_INT(2, (int)utf8_encode(0x80, out));
    TEST_ASSERT_EQUAL_INT(2, (int)utf8_encode(0x7FF, out));
    TEST_ASSERT_EQUAL_INT(3, (int)utf8_encode(0x800, out));
    TEST_ASSERT_EQUAL_INT(3, (int)utf8_encode(0xFFFF, out));
    TEST_ASSERT_EQUAL_INT(4, (int)utf8_encode(0x10000, out));
    TEST_ASSERT_EQUAL_INT(4, (int)utf8_encode(0x10FFFF, out));
}

// Главное свойство: что закодировали, то и разобрали. Проверяется на наборе,
// где есть все четыре длины, - именно так ошибка в сдвиге и вылезет.
static void test_encode_feed_roundtrip(void)
{
    static const uint32_t kCodes[] = {
        'A', 0x7F, 0x80, 0x0416, 0x044F, 0x0401, 0x20AC, 0x1F600, 0x10FFFF,
    };

    for (size_t i = 0; i < sizeof(kCodes) / sizeof(kCodes[0]); ++i) {
        char out[4];
        const size_t n = utf8_encode(kCodes[i], out);
        TEST_ASSERT_TRUE(n > 0);

        utf8_reset(&s_dec);
        utf8_status st = UTF8_PENDING;
        for (size_t b = 0; b < n; ++b) {
            st = utf8_feed(&s_dec, (uint8_t)out[b], &s_out);
        }
        TEST_ASSERT_EQUAL_INT(UTF8_DONE, st);
        TEST_ASSERT_EQUAL_HEX32(kCodes[i], s_out);
    }
}

// То, что нельзя закодировать, кодировщик отвергает так же, как декодер:
// суррогаты и выход за 0x10FFFF.
static void test_encode_rejects_invalid(void)
{
    char out[4];

    TEST_ASSERT_EQUAL_INT(0, (int)utf8_encode(0xD800, out));
    TEST_ASSERT_EQUAL_INT(0, (int)utf8_encode(0xDFFF, out));
    TEST_ASSERT_EQUAL_INT(0, (int)utf8_encode(0x110000, out));
    TEST_ASSERT_EQUAL_INT(0, (int)utf8_encode(0xFFFFFFFFu, out));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_ascii_passes_through);

    RUN_TEST(test_cyrillic_a_two_bytes);
    RUN_TEST(test_cyrillic_lowercase_ya);
    RUN_TEST(test_cyrillic_yo);
    RUN_TEST(test_cyrillic_word_stream);

    RUN_TEST(test_three_byte);
    RUN_TEST(test_four_byte);

    RUN_TEST(test_orphan_continuation);
    RUN_TEST(test_c0_c1_rejected);
    RUN_TEST(test_f5_and_ff_rejected);
    RUN_TEST(test_overlong_nul_rejected);
    RUN_TEST(test_overlong_slash_rejected);
    RUN_TEST(test_surrogate_rejected);
    RUN_TEST(test_broken_tail_recovers);

    RUN_TEST(test_state_survives_calls);
    RUN_TEST(test_reset_drops_pending);

    RUN_TEST(test_encode_ascii_one_byte);
    RUN_TEST(test_encode_cyrillic_two_bytes);
    RUN_TEST(test_encode_length_boundaries);
    RUN_TEST(test_encode_feed_roundtrip);
    RUN_TEST(test_encode_rejects_invalid);

    return UNITY_END();
}
