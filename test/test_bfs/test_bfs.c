// Тесты файловой системы на хосте, поверх RAM-диска.
//
// Почему на маке: ошибка в разметке или в карте блоков на плате стоит
// испорченного раздела и вечера на разбор дампа, а здесь - секунды. Плюс
// главное, чего на живой плате не проверить иначе: обрыв питания.
// RAM-диск умеет «выключиться» после N операций записи, и тогда видно, что
// после перемонтирования ФС осталась целой, а файл - либо старым, либо новым,
// но не половинкой.

#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bfs.h"

void setUp(void)
{
}

void tearDown(void)
{
}

// ---------------------------------------------------------------------------
// RAM-диск
//
// budget < 0 - операций сколько угодно. budget == 0 - «свет выключили»:
// стирание и запись молча не доходят до «flash», чтение работает как обычно.
// Молча, а не с ошибкой, потому что при обрыве питания никто не возвращает
// код: данные просто не записались.
// ---------------------------------------------------------------------------

#define TEST_BLOCKS 64u

typedef struct {
    uint8_t* data;
    uint32_t blocks;
    int budget;
} ram_disk_t;

static uint8_t s_storage[TEST_BLOCKS * BFS_BLOCK_SIZE];
static ram_disk_t s_disk;

static bool budget_take(ram_disk_t* d)
{
    if (d->budget < 0) {
        return true;
    }
    if (d->budget == 0) {
        return false;
    }
    --d->budget;
    return true;
}

static int ram_read(void* ctx, uint32_t off, void* dst, uint32_t len)
{
    ram_disk_t* const d = (ram_disk_t*)ctx;
    if (off + len > d->blocks * BFS_BLOCK_SIZE) {
        return -1;
    }
    memcpy(dst, d->data + off, len);
    return 0;
}

static int ram_erase(void* ctx, uint32_t off)
{
    ram_disk_t* const d = (ram_disk_t*)ctx;
    if (!budget_take(d)) {
        return 0;
    }
    if (off % BFS_BLOCK_SIZE != 0 || off + BFS_BLOCK_SIZE > d->blocks * BFS_BLOCK_SIZE) {
        return -1;
    }
    memset(d->data + off, 0xFF, BFS_BLOCK_SIZE);
    return 0;
}

static int ram_write(void* ctx, uint32_t off, const void* src, uint32_t len)
{
    ram_disk_t* const d = (ram_disk_t*)ctx;
    if (!budget_take(d)) {
        return 0;
    }
    if (off + len > d->blocks * BFS_BLOCK_SIZE) {
        return -1;
    }
    memcpy(d->data + off, src, len);
    return 0;
}

static bfs_device_t device_for(ram_disk_t* d)
{
    bfs_device_t dev;
    dev.ctx = d;
    dev.read = ram_read;
    dev.erase = ram_erase;
    dev.write = ram_write;
    return dev;
}

static void disk_reset(void)
{
    memset(s_storage, 0xFF, sizeof(s_storage));
    s_disk.data = s_storage;
    s_disk.blocks = TEST_BLOCKS;
    s_disk.budget = -1;
}

static bfs_t s_fs;

static void mount_fresh(void)
{
    disk_reset();
    bfs_device_t dev = device_for(&s_disk);
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_format(&s_fs, &dev, TEST_BLOCKS));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&s_fs, &dev));
}

static void write_text(const char* path, const char* text)
{
    const uint32_t len = (uint32_t)strlen(text);
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_write(&s_fs, path, 0, text, len, &done));
    TEST_ASSERT_EQUAL_UINT32(len, done);
}

static void write_pattern(const char* path, uint32_t len, uint8_t seed)
{
    static uint8_t buf[BFS_BLOCK_SIZE * 3];
    TEST_ASSERT_TRUE(len <= sizeof(buf));
    for (uint32_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)(seed + i * 7u);
    }
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_write(&s_fs, path, 0, buf, len, &done));
    TEST_ASSERT_EQUAL_UINT32(len, done);
}

static void check_pattern(const char* path, uint32_t len, uint8_t seed)
{
    static uint8_t buf[BFS_BLOCK_SIZE * 3];
    TEST_ASSERT_TRUE(len <= sizeof(buf));
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, path, 0, buf, len, &done));
    TEST_ASSERT_EQUAL_UINT32(len, done);
    for (uint32_t i = 0; i < len; ++i) {
        if (buf[i] != (uint8_t)(seed + i * 7u)) {
            TEST_FAIL_MESSAGE("содержимое файла не совпало с записанным");
        }
    }
}

// ---------------------------------------------------------------------------
// Разметка и монтирование
// ---------------------------------------------------------------------------

void test_mount_unformatted_fails(void)
{
    disk_reset();
    bfs_device_t dev = device_for(&s_disk);
    bfs_t fs;
    TEST_ASSERT_EQUAL_INT(BFS_ERR_FORMAT, bfs_mount(&fs, &dev));
}

void test_format_then_mount(void)
{
    mount_fresh();
    uint32_t size = 0xFFFFFFFFu;
    uint16_t type = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/", &size, &type));
    TEST_ASSERT_EQUAL_UINT16(BFS_TYPE_DIR, type);
    TEST_ASSERT_EQUAL_UINT32(0, size);

    uint32_t used = 0, total = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &used, &total));
    TEST_ASSERT_EQUAL_UINT32(TEST_BLOCKS, total);
    // 2 сектора суперблока + 8 блоков inode-таблицы + блок корневого каталога.
    TEST_ASSERT_EQUAL_UINT32(11, used);
}

void test_mount_reads_back_geometry(void)
{
    mount_fresh();
    bfs_device_t dev = device_for(&s_disk);
    bfs_t again;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&again, &dev));
    TEST_ASSERT_EQUAL_UINT32(TEST_BLOCKS, again.block_count);
    TEST_ASSERT_EQUAL_UINT32(BFS_ROOT_INODE, again.root_inode);
}

// ---------------------------------------------------------------------------
// Файлы
// ---------------------------------------------------------------------------

void test_create_and_stat(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/rc"));
    uint32_t size = 0xFFFFFFFFu;
    uint16_t type = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/rc", &size, &type));
    TEST_ASSERT_EQUAL_UINT16(BFS_TYPE_FILE, type);
    TEST_ASSERT_EQUAL_UINT32(0, size);
}

void test_create_existing_fails(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/rc"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_EXISTS, bfs_create(&s_fs, "/rc"));
}

void test_stat_missing_fails(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NOT_FOUND, bfs_stat(&s_fs, "/nope", NULL, NULL));
}

void test_write_read_roundtrip(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/hello"));
    write_text("/hello", "привет, мир");
    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/hello", &size, NULL));
    TEST_ASSERT_EQUAL_UINT32(strlen("привет, мир"), size);

    char buf[64];
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/hello", 0, buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_UINT32(strlen("привет, мир"), done);
    // Read не дописывает ноль: длину знает вызывающий, а лишний байт в файле
    // был бы не отличим от содержимого.
    buf[done] = '\0';
    TEST_ASSERT_EQUAL_STRING("привет, мир", buf);
}

// Запись длиннее блока: проверяет, что карта блоков и переход через границу
// сектора работают.
void test_write_crosses_block(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/big"));
    write_pattern("/big", 5000, 3);
    check_pattern("/big", 5000, 3);
}

void test_overwrite_middle(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/f"));
    write_pattern("/f", 5000, 11);

    static uint8_t patch[200];
    memset(patch, 0x5A, sizeof(patch));
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_write(&s_fs, "/f", 100, patch, sizeof(patch), &done));
    TEST_ASSERT_EQUAL_UINT32(sizeof(patch), done);

    static uint8_t buf[5000];
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/f", 0, buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_UINT32(5000, done);
    for (uint32_t i = 0; i < 100; ++i) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(11 + i * 7u), buf[i]);
    }
    for (uint32_t i = 100; i < 300; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0x5A, buf[i]);
    }
    for (uint32_t i = 300; i < 5000; ++i) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(11 + i * 7u), buf[i]);
    }
}

void test_read_past_eof_gives_nothing(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/f"));
    write_text("/f", "abc");
    char buf[16];
    uint32_t done = 12345;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/f", 3, buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_UINT32(0, done);
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/f", 1, buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_UINT32(2, done);
    TEST_ASSERT_EQUAL_MEMORY("bc", buf, 2);
}

// Indirect-блок появляется на 13-м блоке файла (12 прямых). Каждый блок
// заливается своим значком, чтобы проверка не зависела от разбиения на куски.
void test_indirect_blocks(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/long"));

    const uint32_t blocks = BFS_DIRECT_BLOCKS + 1u;
    static uint8_t chunk[BFS_BLOCK_SIZE];
    for (uint32_t b = 0; b < blocks; ++b) {
        memset(chunk, (int)(0xC0u + b), sizeof(chunk));
        uint32_t done = 0;
        TEST_ASSERT_EQUAL_INT(
            BFS_OK, bfs_write(&s_fs, "/long", b * BFS_BLOCK_SIZE, chunk, sizeof(chunk), &done));
        TEST_ASSERT_EQUAL_UINT32(sizeof(chunk), done);
    }

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/long", &size, NULL));
    TEST_ASSERT_EQUAL_UINT32(blocks * BFS_BLOCK_SIZE, size);

    // Последний блок адресуется уже через indirect: прямые кончились на 12-м.
    const uint32_t off = BFS_DIRECT_BLOCKS * BFS_BLOCK_SIZE;
    uint8_t probe = 0;
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/long", off, &probe, 1, &done));
    TEST_ASSERT_EQUAL_UINT32(1, done);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(0xC0u + BFS_DIRECT_BLOCKS), probe);

    // После перемонтирования indirect-блок должен находиться по inode.
    bfs_device_t dev = device_for(&s_disk);
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&s_fs, &dev));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/long", off, &probe, 1, &done));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(0xC0u + BFS_DIRECT_BLOCKS), probe);
}

// Разреженная запись далеко за 4 МБ: дырка не занимает блоков, но трогает
// double indirect. Так проверяется третий уровень карты без записи 4 МБ.
void test_double_indirect_sparse(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/sparse"));

    const uint32_t off = (BFS_DIRECT_BLOCKS + BFS_PTRS_PER_BLOCK + 5u) * BFS_BLOCK_SIZE;
    const char payload[] = "far away";
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK,
                          bfs_write(&s_fs, "/sparse", off, payload, sizeof(payload), &done));
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), done);

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/sparse", &size, NULL));
    TEST_ASSERT_EQUAL_UINT32(off + sizeof(payload), size);

    char buf[sizeof(payload)];
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/sparse", off, buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_MEMORY(payload, buf, sizeof(payload));

    // Дырка перед записью читается нулями, а не 0xFF из стёртой flash.
    uint8_t hole[8];
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/sparse", 0, hole, sizeof(hole), &done));
    for (size_t i = 0; i < sizeof(hole); ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, hole[i]);
    }

    // Усечение до нуля освобождает и блок данных, и оба промежуточных.
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_truncate(&s_fs, "/sparse", 0));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/sparse", &size, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, size);
}

void test_truncate_shrinks_and_frees(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/f"));
    write_pattern("/f", 9000, 5);

    uint32_t used_before = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &used_before, NULL));

    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_truncate(&s_fs, "/f", 100));
    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/f", &size, NULL));
    TEST_ASSERT_EQUAL_UINT32(100, size);

    uint32_t used_after = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &used_after, NULL));
    TEST_ASSERT_TRUE(used_after < used_before);

    // После перемонтирования освобождение должно сохраниться: указатели
    // обнулены, обход не считает эти блоки занятыми.
    bfs_device_t dev = device_for(&s_disk);
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&s_fs, &dev));
    uint32_t used_remount = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &used_remount, NULL));
    TEST_ASSERT_EQUAL_UINT32(used_after, used_remount);
}

void test_bad_name(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NAME,
                          bfs_create(&s_fs, "/0123456789012345678901234"));  // 25 > 24
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NAME, bfs_create(&s_fs, "/"));
}

void test_no_space(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/huge"));
    static uint8_t chunk[BFS_BLOCK_SIZE];
    memset(chunk, 0xA5, sizeof(chunk));

    int rc = BFS_OK;
    for (uint32_t i = 0; i < TEST_BLOCKS && rc == BFS_OK; ++i) {
        uint32_t done = 0;
        rc = bfs_write(&s_fs, "/huge", i * BFS_BLOCK_SIZE, chunk, sizeof(chunk), &done);
    }
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// Каталоги и переименование
// ---------------------------------------------------------------------------

void test_mkdir_and_nested_create(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/etc"));
    uint16_t type = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/etc", NULL, &type));
    TEST_ASSERT_EQUAL_UINT16(BFS_TYPE_DIR, type);

    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/etc/rc"));
    write_text("/etc/rc", "echo hi");
    char buf[16];
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/etc/rc", 0, buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_UINT32(strlen("echo hi"), done);
    TEST_ASSERT_EQUAL_STRING("echo hi", buf);

    TEST_ASSERT_EQUAL_INT(BFS_ERR_EXISTS, bfs_mkdir(&s_fs, "/etc"));
}

void test_create_under_file_fails(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/f"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NOT_DIR, bfs_create(&s_fs, "/f/x"));
}

void test_readdir_skips_tombstones(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/a"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/b"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/c"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_unlink(&s_fs, "/b"));

    char name[BFS_NAME_MAX + 1];
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_readdir(&s_fs, "/", 0, name, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("a", name);
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_readdir(&s_fs, "/", 1, name, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("c", name);
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NOT_FOUND, bfs_readdir(&s_fs, "/", 2, name, NULL, NULL));

    // Освободившееся место переиспользуется: новый файл встаёт на место "b",
    // и порядок обхода снова a, b(новый), c.
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/d"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_readdir(&s_fs, "/", 1, name, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("d", name);
}

void test_unlink_frees_blocks(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/f"));
    write_pattern("/f", 9000, 7);
    uint32_t used_with_file = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &used_with_file, NULL));

    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_unlink(&s_fs, "/f"));
    uint32_t used_after = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &used_after, NULL));
    TEST_ASSERT_TRUE(used_after < used_with_file);

    bfs_device_t dev = device_for(&s_disk);
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&s_fs, &dev));
    uint32_t used_remount = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &used_remount, NULL));
    TEST_ASSERT_EQUAL_UINT32(used_after, used_remount);
}

// Удаление каталога обязано возвращать его блоки в оборот: иначе каждый
// mkdir/rmdir терял бы блок навсегда, и место кончалось бы без файлов.
void test_rmdir_frees_blocks(void)
{
    mount_fresh();
    uint32_t baseline = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &baseline, NULL));

    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/etc"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/etc/rc.d"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_rmdir(&s_fs, "/etc/rc.d"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_rmdir(&s_fs, "/etc"));

    uint32_t after = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &after, NULL));
    TEST_ASSERT_EQUAL_UINT32(baseline, after);

    // И после перемонтирования: битмап собирается обходом, и освобождение
    // должно быть видно уже из диска, а не только из памяти.
    bfs_device_t dev = device_for(&s_disk);
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&s_fs, &dev));
    uint32_t remount = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &remount, NULL));
    TEST_ASSERT_EQUAL_UINT32(baseline, remount);
}

void test_unlink_directory_fails(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/etc"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_IS_DIR, bfs_unlink(&s_fs, "/etc"));
}

void test_rmdir_non_empty_fails(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/etc"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/etc/rc"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NOT_EMPTY, bfs_rmdir(&s_fs, "/etc"));

    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_unlink(&s_fs, "/etc/rc"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_rmdir(&s_fs, "/etc"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NOT_FOUND, bfs_stat(&s_fs, "/etc", NULL, NULL));
}

void test_rename_same_dir(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/old"));
    write_text("/old", "data");
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_rename(&s_fs, "/old", "/new"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NOT_FOUND, bfs_stat(&s_fs, "/old", NULL, NULL));

    char buf[8];
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&s_fs, "/new", 0, buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_UINT32(4, done);
    TEST_ASSERT_EQUAL_MEMORY("data", buf, 4);
}

void test_rename_across_dirs(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/a"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/b"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/a/f"));
    write_text("/a/f", "x");

    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_rename(&s_fs, "/a/f", "/b/f"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NOT_FOUND, bfs_stat(&s_fs, "/a/f", NULL, NULL));
    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/b/f", &size, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, size);
}

void test_rename_to_existing_fails(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/a"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/b"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_EXISTS, bfs_rename(&s_fs, "/a", "/b"));
}

// Каталог внутрь себя - это дерево, у которого узел является своим потомком:
// обход по нему не заканчивается. Поэтому отказ, а не «сделали и не заметили».
void test_rename_dir_into_itself_fails(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/a"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/a/b"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_ARG, bfs_rename(&s_fs, "/a", "/a/b/a"));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_ARG, bfs_rename(&s_fs, "/a", "/a"));

    // Соседнее имя с тем же началом - не потомок, переименовывать можно.
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_rename(&s_fs, "/a", "/ab"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/ab/b", NULL, NULL));
}

// Путь через файл - не NOT_FOUND, а NOT_DIR: иначе содержимое файла
// разбиралось бы как записи каталога.
void test_path_through_file_fails(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/f"));
    write_text("/f", "не каталог");
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NOT_DIR, bfs_stat(&s_fs, "/f/x", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(BFS_ERR_NOT_DIR, bfs_create(&s_fs, "/f/x"));
}

// ---------------------------------------------------------------------------
// Переживание перезагрузки и обрыва питания
// ---------------------------------------------------------------------------

void test_data_survives_remount(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/etc"));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/etc/rc"));
    write_text("/etc/rc", "echo basalt");
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/data"));
    write_pattern("/data", 6000, 17);

    bfs_device_t dev = device_for(&s_disk);
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&s_fs, &dev));

    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&s_fs, "/etc/rc", &size, NULL));
    TEST_ASSERT_EQUAL_UINT32(strlen("echo basalt"), size);
    check_pattern("/data", 6000, 17);
}

// Главный тест: обрыв питания в любой точке записи.
//
// Файл уже лежит на диске (два блока). Затем пишется третий блок, и «питание
// пропадает» после k операций стирания/записи. После перемонтирования ФС
// обязана монтироваться, а файл - быть либо полностью старым, либо полностью
// новым. Половинки быть не может: коммит - это запись суперблока целиком.
void test_power_cut_leaves_consistent_state(void)
{
    const uint32_t old_len = 2u * BFS_BLOCK_SIZE;
    const uint32_t new_len = 3u * BFS_BLOCK_SIZE;

    for (int budget = 0; budget <= 24; ++budget) {
        mount_fresh();
        TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, "/f"));
        write_pattern("/f", old_len, 21);

        // Питание «пропадает» после budget операций стирания и записи.
        s_disk.budget = budget;
        static uint8_t extra[BFS_BLOCK_SIZE];
        memset(extra, 0x33, sizeof(extra));
        bfs_write(&s_fs, "/f", old_len, extra, sizeof(extra), NULL);

        s_disk.budget = -1;
        bfs_device_t dev = device_for(&s_disk);
        bfs_t fs;
        TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&fs, &dev));

        uint32_t size = 0;
        TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_stat(&fs, "/f", &size, NULL));
        TEST_ASSERT_TRUE(size == old_len || size == new_len);

        static uint8_t buf[3u * BFS_BLOCK_SIZE];
        uint32_t done = 0;
        TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_read(&fs, "/f", 0, buf, size, &done));
        TEST_ASSERT_EQUAL_UINT32(size, done);
        for (uint32_t i = 0; i < old_len; ++i) {
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(21 + i * 7u), buf[i]);
        }
        if (size == new_len) {
            for (uint32_t i = old_len; i < new_len; ++i) {
                TEST_ASSERT_EQUAL_UINT8(0x33, buf[i]);
            }
        }
    }
}

// Обрыв во время создания файла: либо файла нет, либо он есть и пуст.
void test_power_cut_during_create(void)
{
    for (int budget = 0; budget <= 12; ++budget) {
        mount_fresh();
        s_disk.budget = budget;
        bfs_create(&s_fs, "/new");
        s_disk.budget = -1;

        bfs_device_t dev = device_for(&s_disk);
        bfs_t fs;
        TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&fs, &dev));

        uint32_t size = 12345;
        const int rc = bfs_stat(&fs, "/new", &size, NULL);
        TEST_ASSERT_TRUE(rc == BFS_OK || rc == BFS_ERR_NOT_FOUND);
        if (rc == BFS_OK) {
            TEST_ASSERT_EQUAL_UINT32(0, size);
        }
    }
}

// Битмап после перемонтирования должен совпадать с тем, что был: он строится
// обходом inode, и если обход чего-то не увидел, свободные блоки начнут
// выдаваться повторно.
void test_usage_stable_across_remount(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mkdir(&s_fs, "/d"));
    for (int i = 0; i < 5; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/d/f%d", i);
        TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_create(&s_fs, path));
        write_pattern(path, 1000, (uint8_t)i);
    }
    uint32_t used = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &used, NULL));

    bfs_device_t dev = device_for(&s_disk);
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&s_fs, &dev));
    uint32_t used2 = 0;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_usage(&s_fs, &used2, NULL));
    TEST_ASSERT_EQUAL_UINT32(used, used2);
}

// ---------------------------------------------------------------------------

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_mount_unformatted_fails);
    RUN_TEST(test_format_then_mount);
    RUN_TEST(test_mount_reads_back_geometry);

    RUN_TEST(test_create_and_stat);
    RUN_TEST(test_create_existing_fails);
    RUN_TEST(test_stat_missing_fails);
    RUN_TEST(test_write_read_roundtrip);
    RUN_TEST(test_write_crosses_block);
    RUN_TEST(test_overwrite_middle);
    RUN_TEST(test_read_past_eof_gives_nothing);
    RUN_TEST(test_indirect_blocks);
    RUN_TEST(test_double_indirect_sparse);
    RUN_TEST(test_truncate_shrinks_and_frees);
    RUN_TEST(test_bad_name);
    RUN_TEST(test_no_space);

    RUN_TEST(test_mkdir_and_nested_create);
    RUN_TEST(test_create_under_file_fails);
    RUN_TEST(test_readdir_skips_tombstones);
    RUN_TEST(test_unlink_frees_blocks);
    RUN_TEST(test_rmdir_frees_blocks);
    RUN_TEST(test_unlink_directory_fails);
    RUN_TEST(test_rmdir_non_empty_fails);
    RUN_TEST(test_rename_same_dir);
    RUN_TEST(test_rename_across_dirs);
    RUN_TEST(test_rename_to_existing_fails);
    RUN_TEST(test_rename_dir_into_itself_fails);
    RUN_TEST(test_path_through_file_fails);

    RUN_TEST(test_data_survives_remount);
    RUN_TEST(test_power_cut_leaves_consistent_state);
    RUN_TEST(test_power_cut_during_create);
    RUN_TEST(test_usage_stable_across_remount);

    return UNITY_END();
}
