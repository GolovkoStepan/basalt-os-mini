// Тесты приёма XMODEM на маке.
//
// Почему здесь: протокол - это арифметика над байтами. Проверять её на живой
// плате пришлось бы вручную: набери команду, запусти отправитель, посмотри,
// что файл побайтово тот же. Повторить сценарий «ACK потерялся» или «пакет
// пришёл с битой CRC» на железе почти невозможно, а в скрипте потока они
// занимают три строки. Плата проверяет только провод: консоль и запись в ФС.
//
// Поток отправителя задаётся заранее массивом: сокетный тест читал бы
// отправителя, а нам важен разбор того, что он присылает.

#include <unity.h>

#include <string.h>

#include "xmodem.h"

void setUp(void)
{
}

void tearDown(void)
{
}

#define IN_MAX   (40000u)
#define FILE_MAX (35000u)

typedef struct {
    uint8_t in[IN_MAX];
    size_t in_len;
    size_t in_pos;

    uint8_t out[512];
    size_t out_len;

    uint8_t file[FILE_MAX];
    size_t file_len;

    int sink_rc;  // что вернуть из sink: 0 - успех
    uint32_t sink_calls;
    uint32_t timeouts;

    int finish_rc;
    uint32_t finish_calls;
    size_t finish_out_len;  // сколько уже отправлено в момент вызова finish
} fake_io;

static fake_io s_fake;

static int fake_put(void* ctx, const uint8_t* data, size_t len)
{
    fake_io* f = ctx;
    if (len > sizeof(f->out) - f->out_len) {
        len = sizeof(f->out) - f->out_len;
    }
    memcpy(f->out + f->out_len, data, len);
    f->out_len += len;
    return (int)len;
}

static int fake_get(void* ctx, uint8_t* data, size_t len, uint32_t timeout_ms)
{
    fake_io* f = ctx;
    if (timeout_ms == 0) {
        return 0;  // «только то, что уже лежит»: в тестах буфер пуст
    }

    size_t n = f->in_len - f->in_pos;
    if (n > len) {
        n = len;
    }
    if (n == 0) {
        ++f->timeouts;
        return 0;
    }
    memcpy(data, f->in + f->in_pos, n);
    f->in_pos += n;
    return (int)n;
}

static int fake_sink(void* ctx, const uint8_t* data, size_t len)
{
    fake_io* f = ctx;
    ++f->sink_calls;
    if (f->sink_rc != 0) {
        return f->sink_rc;
    }
    if (len > FILE_MAX - f->file_len) {
        return -1;
    }
    memcpy(f->file + f->file_len, data, len);
    f->file_len += len;
    return 0;
}

static int fake_finish(void* ctx)
{
    fake_io* f = ctx;
    ++f->finish_calls;
    f->finish_out_len = f->out_len;
    return f->finish_rc;
}

static xmodem_io make_io(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    xmodem_io io;
    io.ctx = &s_fake;
    io.put = fake_put;
    io.get = fake_get;
    io.sink = fake_sink;
    io.finish = fake_finish;
    return io;
}

static uint16_t crc16_xmodem(const uint8_t* data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// Положить в поток пакет так, как его пришлёт отправитель. Возвращает индекс
// первого байта пакета - по нему тесты портят нужное поле.
static size_t push_packet(uint8_t seq, const uint8_t* data, size_t len)
{
    fake_io* f = &s_fake;
    const size_t start = f->in_len;

    f->in[f->in_len++] = (len == 1024u) ? 0x02u : 0x01u;
    f->in[f->in_len++] = seq;
    f->in[f->in_len++] = (uint8_t)~seq;

    memcpy(f->in + f->in_len, data, len);
    f->in_len += len;

    const uint16_t crc = crc16_xmodem(data, len);
    f->in[f->in_len++] = (uint8_t)(crc >> 8);
    f->in[f->in_len++] = (uint8_t)(crc & 0xFFu);
    return start;
}

// Пакет, заполненный одним байтом. Промежуточные пакеты XMODEM всегда полные:
// дополнение 0x1A бывает только в последнем.
static void push_filled(uint8_t seq, uint8_t fill, size_t len)
{
    uint8_t block[1024];
    TEST_ASSERT_TRUE(len <= sizeof(block));
    memset(block, fill, len);
    push_packet(seq, block, len);
}

// Пакет с текстом и дополнением 0x1A до конца блока - так выглядит последний
// пакет с текстовым файлом.
static void push_text_packet(uint8_t seq, const char* text)
{
    uint8_t block[128];
    const size_t n = strlen(text);
    TEST_ASSERT_TRUE(n <= sizeof(block));
    memcpy(block, text, n);
    memset(block + n, 0x1A, sizeof(block) - n);
    push_packet(seq, block, sizeof(block));
}

static void push_eot(void)
{
    s_fake.in[s_fake.in_len++] = 0x04u;
}

static void assert_out(const uint8_t* expected, size_t len)
{
    TEST_ASSERT_EQUAL_UINT32(len, s_fake.out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, s_fake.out, len);
}

static void assert_file(const uint8_t* expected, size_t len)
{
    TEST_ASSERT_EQUAL_UINT32(len, s_fake.file_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, s_fake.file, len);
}

static void assert_ends_with_cancel(void)
{
    TEST_ASSERT_TRUE(s_fake.out_len >= 2);
    TEST_ASSERT_EQUAL_UINT8(0x18, s_fake.out[s_fake.out_len - 2u]);
    TEST_ASSERT_EQUAL_UINT8(0x18, s_fake.out[s_fake.out_len - 1u]);
}

static void test_one_packet_and_eot(void)
{
    const xmodem_io io = make_io();
    push_text_packet(1, "привет");
    push_eot();

    uint32_t size = 12345;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));

    TEST_ASSERT_EQUAL_UINT32(12, size);  // «привет» в UTF-8
    assert_file((const uint8_t*)"привет", 12);

    // 'C', ACK за пакет, ACK за конец.
    const uint8_t expected[] = {'C', 0x06, 0x06};
    assert_out(expected, sizeof(expected));
}

static void test_padding_is_dropped(void)
{
    const xmodem_io io = make_io();

    // В пакете «ab», 0x1A как настоящий байт данных, «cd» и дополнение.
    uint8_t block[128];
    memset(block, 0x1A, sizeof(block));
    memcpy(block, "ab\x1a" "cd", 5);
    const uint8_t want[] = {'a', 'b', 0x1A, 'c', 'd'};
    push_packet(1, block, sizeof(block));
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), size);
    assert_file(want, sizeof(want));
}

static void test_all_padding_gives_empty_file(void)
{
    const xmodem_io io = make_io();
    push_filled(1, 0x1A, 128);
    push_eot();

    uint32_t size = 99;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(0, size);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.file_len);
}

static void test_empty_file_is_eot_right_away(void)
{
    const xmodem_io io = make_io();
    push_eot();

    uint32_t size = 99;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(0, size);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.file_len);

    const uint8_t expected[] = {'C', 0x06};
    assert_out(expected, sizeof(expected));
}

static void test_1024_byte_packet(void)
{
    const xmodem_io io = make_io();

    uint8_t block[1024];
    for (size_t i = 0; i < sizeof(block); ++i) {
        block[i] = (uint8_t)(i * 7u + 3u);
    }
    push_packet(1, block, sizeof(block));
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(sizeof(block), size);
    assert_file(block, sizeof(block));
}

static void test_duplicate_is_acked_but_not_written(void)
{
    const xmodem_io io = make_io();
    push_filled(1, 'A', 128);
    push_filled(1, 'A', 128);  // наш ACK потерялся, отправитель повторил
    push_filled(2, 'B', 128);
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(256, size);
    TEST_ASSERT_EQUAL_UINT8('A', s_fake.file[0]);
    TEST_ASSERT_EQUAL_UINT8('A', s_fake.file[127]);
    TEST_ASSERT_EQUAL_UINT8('B', s_fake.file[128]);
    TEST_ASSERT_EQUAL_UINT8('B', s_fake.file[255]);

    // 'C', три ACK за пакеты (включая повтор), ACK за конец.
    const uint8_t expected[] = {'C', 0x06, 0x06, 0x06, 0x06};
    assert_out(expected, sizeof(expected));
}

static void test_bad_crc_gets_nak_and_resend(void)
{
    const xmodem_io io = make_io();

    uint8_t block[128];
    memcpy(block, "hello", 5);
    memset(block + 5, 0x1A, sizeof(block) - 5);

    push_packet(1, block, sizeof(block));
    s_fake.in[s_fake.in_len - 1u] ^= 0xFFu;  // испорченная CRC

    push_packet(1, block, sizeof(block));
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(5, size);
    assert_file((const uint8_t*)"hello", 5);

    const uint8_t expected[] = {'C', 0x15, 0x06, 0x06};
    assert_out(expected, sizeof(expected));
}

static void test_broken_packets_end_with_error(void)
{
    const xmodem_io io = make_io();
    for (int i = 0; i < 12; ++i) {
        push_filled(1, 'x', 128);
        s_fake.in[s_fake.in_len - 1u] ^= 0xFFu;
    }
    push_eot();

    uint32_t size = 7;
    TEST_ASSERT_EQUAL_INT(XMODEM_ERR_PROTOCOL, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(0, size);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.file_len);
    assert_ends_with_cancel();
}

static void test_sequence_gap_stops_transfer(void)
{
    const xmodem_io io = make_io();
    push_filled(1, 'A', 128);
    push_filled(3, 'C', 128);  // второй пакет потерялся
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_ERR_SEQUENCE, xmodem_receive(&io, &size));
    // Размер отдаётся только при успехе: файл после ошибки всё равно удаляется.
    TEST_ASSERT_EQUAL_UINT32(0, size);
    TEST_ASSERT_EQUAL_UINT32(128, s_fake.file_len);
    assert_ends_with_cancel();
}

static void test_cancel_from_sender(void)
{
    const xmodem_io io = make_io();
    s_fake.in[s_fake.in_len++] = 0x18;
    s_fake.in[s_fake.in_len++] = 0x18;

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_ERR_CANCEL, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.file_len);
}

static void test_lone_can_does_not_eat_next_packet(void)
{
    // Одиночный CAN - это просто байт 0x18 (помеха), а не отмена. Следом идёт
    // настоящий пакет: если принять его заголовок за второй CAN или выбросить,
    // передача развалится на ровном месте.
    const xmodem_io io = make_io();
    s_fake.in[s_fake.in_len++] = 0x18;
    push_text_packet(1, "ok");
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(2, size);
    assert_file((const uint8_t*)"ok", 2);
}

static void test_silence_is_timeout(void)
{
    const xmodem_io io = make_io();

    uint32_t size = 5;
    TEST_ASSERT_EQUAL_INT(XMODEM_ERR_TIMEOUT, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(0, size);

    // Пять попыток начать передачу - пять 'C', и в конце CAN CAN: отправитель,
    // который всё-таки откроет порт, должен сразу остановиться, а не ждать,
    // пока плата ответит на его первый пакет.
    const uint8_t expected[] = {'C', 'C', 'C', 'C', 'C', 0x18, 0x18};
    assert_out(expected, sizeof(expected));
}

static void test_bad_block_number_is_nak(void)
{
    const xmodem_io io = make_io();
    push_filled(1, 'A', 128);

    // Второй пакет с номером 2, у которого испорчена инверсия номера.
    uint8_t block[128];
    memset(block, 'B', sizeof(block));
    const size_t start = push_packet(2, block, sizeof(block));
    s_fake.in[start + 1u] = 2;
    s_fake.in[start + 2u] = 2;  // ~2 должен быть 0xFD

    push_packet(2, block, sizeof(block));
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(256, size);
    TEST_ASSERT_EQUAL_UINT8('A', s_fake.file[0]);
    TEST_ASSERT_EQUAL_UINT8('B', s_fake.file[128]);

    const uint8_t expected[] = {'C', 0x06, 0x15, 0x06, 0x06};
    assert_out(expected, sizeof(expected));
}

static void test_sink_refusal_stops_transfer(void)
{
    const xmodem_io io = make_io();
    push_filled(1, 'A', 128);
    push_eot();

    s_fake.sink_rc = -1;

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_ERR_SINK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(0, size);
}

static void test_padding_longer_than_block_is_flushed(void)
{
    // Пакет кончается настоящим 0x1A, следующий - целый килобайт 0x1A. Хвост
    // перестаёт помещаться в «не длиннее пакета», и лишнее обязано уйти в файл,
    // иначе настоящие данные потерялись бы молча.
    const xmodem_io io = make_io();

    uint8_t block[1024];
    memset(block, 'A', sizeof(block));
    block[sizeof(block) - 1u] = 0x1A;
    push_packet(1, block, sizeof(block));
    push_filled(2, 0x1A, 1024);
    push_filled(3, 'B', 1024);
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(1024u + 1024u + 1024u, size);
    TEST_ASSERT_EQUAL_UINT8(0x1A, s_fake.file[1023]);
    TEST_ASSERT_EQUAL_UINT8(0x1A, s_fake.file[1024]);
    TEST_ASSERT_EQUAL_UINT8('B', s_fake.file[2048]);
}

static void test_block_numbers_wrap_after_255(void)
{
    // 260 пакетов по 128 байт: номера проходят 1..255 и переходят через ноль.
    // Именно здесь ошибка «номер кончился» видна сразу, а не через полгода.
    const xmodem_io io = make_io();

    for (uint32_t i = 1; i <= 260; ++i) {
        push_filled((uint8_t)i, 'A', 128);
    }
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(260u * 128u, size);
    TEST_ASSERT_EQUAL_UINT32(260u * 128u, s_fake.file_len);
}

static void test_finish_runs_before_final_ack(void)
{
    const xmodem_io io = make_io();
    push_text_packet(1, "data");
    push_eot();

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_OK, xmodem_receive(&io, &size));
    TEST_ASSERT_EQUAL_UINT32(1, s_fake.finish_calls);

    // 'C' и ACK за пакет уже ушли, ACK за EOT - ещё нет: иначе отправитель
    // узнал бы об успехе раньше, чем файл дописан.
    TEST_ASSERT_EQUAL_UINT32(2, s_fake.finish_out_len);
    TEST_ASSERT_EQUAL_UINT32(3, s_fake.out_len);
}

static void test_finish_refusal_is_sink_error(void)
{
    const xmodem_io io = make_io();
    push_text_packet(1, "data");
    push_eot();
    s_fake.finish_rc = -1;

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(XMODEM_ERR_SINK, xmodem_receive(&io, &size));
    assert_ends_with_cancel();
}

static void test_missing_argument_is_refused(void)
{
    uint32_t size = 1;
    TEST_ASSERT_EQUAL_INT(XMODEM_ERR_ARG, xmodem_receive(NULL, &size));
    TEST_ASSERT_EQUAL_UINT32(0, size);

    xmodem_io io = make_io();
    io.sink = NULL;
    TEST_ASSERT_EQUAL_INT(XMODEM_ERR_ARG, xmodem_receive(&io, NULL));

    io = make_io();
    io.finish = NULL;
    TEST_ASSERT_EQUAL_INT(XMODEM_ERR_ARG, xmodem_receive(&io, NULL));
}

static void test_errors_have_russian_text(void)
{
    TEST_ASSERT_NOT_NULL(xmodem_strerror(XMODEM_ERR_TIMEOUT));
    TEST_ASSERT_NOT_NULL(xmodem_strerror(XMODEM_ERR_CANCEL));
    TEST_ASSERT_NOT_NULL(xmodem_strerror(-999));
    TEST_ASSERT_TRUE(strlen(xmodem_strerror(-999)) > 0);
}

int main(int argc, char** argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_one_packet_and_eot);
    RUN_TEST(test_padding_is_dropped);
    RUN_TEST(test_all_padding_gives_empty_file);
    RUN_TEST(test_empty_file_is_eot_right_away);
    RUN_TEST(test_1024_byte_packet);
    RUN_TEST(test_duplicate_is_acked_but_not_written);
    RUN_TEST(test_bad_crc_gets_nak_and_resend);
    RUN_TEST(test_broken_packets_end_with_error);
    RUN_TEST(test_sequence_gap_stops_transfer);
    RUN_TEST(test_cancel_from_sender);
    RUN_TEST(test_lone_can_does_not_eat_next_packet);
    RUN_TEST(test_silence_is_timeout);
    RUN_TEST(test_bad_block_number_is_nak);
    RUN_TEST(test_sink_refusal_stops_transfer);
    RUN_TEST(test_padding_longer_than_block_is_flushed);
    RUN_TEST(test_block_numbers_wrap_after_255);
    RUN_TEST(test_finish_runs_before_final_ack);
    RUN_TEST(test_finish_refusal_is_sink_error);
    RUN_TEST(test_missing_argument_is_refused);
    RUN_TEST(test_errors_have_russian_text);

    return UNITY_END();
}
