// Тесты VFS на хосте: канонизация пути, точки монтирования, открытые файлы.
//
// Почему на маке. Разбор пути - самая частая и самая незаметная ошибка:
// «/a/../b» тихо открывает не тот файл, и узнаётся об этом через месяц.
// Проверять это на плате - значит перепрошивать её ради каждой догадки.
//
// Второй слой тестов идёт через настоящую BFS на RAM-диске (так же, как
// test_bfs): VFS и адаптер проверяются вместе, потому что по отдельности они
// бессмысленны - вся их работа в том, как путь доходит до ФС.

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "bfs.h"
#include "vfs.h"
#include "vfs_bfs.h"

void setUp(void)
{
}

void tearDown(void)
{
}

// ---------------------------------------------------------------------------
// RAM-диск и смонтированная ФС
// ---------------------------------------------------------------------------

#define TEST_BLOCKS 64u

static uint8_t s_storage[TEST_BLOCKS * BFS_BLOCK_SIZE];

static int ram_read(void* ctx, uint32_t off, void* dst, uint32_t len)
{
    (void)ctx;
    if (off + len > sizeof(s_storage)) {
        return -1;
    }
    memcpy(dst, s_storage + off, len);
    return 0;
}

static int ram_erase(void* ctx, uint32_t off)
{
    (void)ctx;
    if (off % BFS_BLOCK_SIZE != 0 || off + BFS_BLOCK_SIZE > sizeof(s_storage)) {
        return -1;
    }
    memset(s_storage + off, 0xFF, BFS_BLOCK_SIZE);
    return 0;
}

static int ram_write(void* ctx, uint32_t off, const void* src, uint32_t len)
{
    (void)ctx;
    if (off + len > sizeof(s_storage)) {
        return -1;
    }
    memcpy(s_storage + off, src, len);
    return 0;
}

static bfs_t s_fs;

// Свежий раздел, смонтированный в "/" - то же состояние, что у системы после
// первой разметки.
static void mount_fresh(void)
{
    memset(s_storage, 0xFF, sizeof(s_storage));

    bfs_device_t dev;
    dev.ctx = NULL;
    dev.read = ram_read;
    dev.erase = ram_erase;
    dev.write = ram_write;
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_format(&s_fs, &dev, TEST_BLOCKS));
    TEST_ASSERT_EQUAL_INT(BFS_OK, bfs_mount(&s_fs, &dev));

    vfs_init();
    const vfs_backend_t be = vfs_bfs_backend(&s_fs);
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/", &be));
}

// ---------------------------------------------------------------------------
// Канонизация пути: чистая логика, без ФС
// ---------------------------------------------------------------------------

static void check_norm(const char* cwd, const char* path, const char* want)
{
    char out[VFS_PATH_MAX];
    const int rc = vfs_normalize(cwd, path, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(VFS_OK, rc);
    if (strcmp(out, want) != 0) {
        TEST_FAIL_MESSAGE(out);
    }
}

void test_normalize_absolute(void)
{
    check_norm("/", "/etc/rc", "/etc/rc");
    check_norm("/", "/", "/");
    check_norm("/", "///", "/");
    check_norm("/", "//etc//rc//", "/etc/rc");
}

void test_normalize_relative(void)
{
    check_norm("/", "etc", "/etc");
    check_norm("/etc", "rc", "/etc/rc");
    check_norm("/etc/", "rc", "/etc/rc");
    check_norm("/etc", "./rc", "/etc/rc");
    check_norm("/etc", ".", "/etc");
    check_norm("/etc", "..", "/");
}

void test_normalize_dotdot(void)
{
    check_norm("/", "/etc/rc/..", "/etc");
    check_norm("/", "/etc/../bin", "/bin");
    check_norm("/a/b/c", "../../..", "/");
    // Выше корня некуда: так же, как в POSIX.
    check_norm("/", "/..", "/");
    check_norm("/", "/../../..", "/");
    check_norm("/", "..", "/");
}

// Точка в имени файла - не компонент ".": "..." и "..hidden" обычные имена.
void test_normalize_dots_in_names(void)
{
    check_norm("/", "/...", "/...");
    check_norm("/", "/..hidden", "/..hidden");
    check_norm("/", "/a.b", "/a.b");
    check_norm("/", "/.rc", "/.rc");
}

void test_normalize_rejects_bad(void)
{
    char out[VFS_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NAME, vfs_normalize("/", "", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_ARG, vfs_normalize("/", NULL, out, sizeof(out)));

    // Не влезает в буфер - отказ, а не обрезанный путь.
    char small[8];
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NAME, vfs_normalize("/", "/etc/passwd", small, sizeof(small)));

    // Длиннее самого предела.
    char big[VFS_PATH_MAX + 32];
    memset(big, 'a', sizeof(big));
    big[0] = '/';
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NAME, vfs_normalize("/", big, out, sizeof(out)));
}

void test_normalize_relative_needs_cwd(void)
{
    char out[VFS_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(VFS_ERR_ARG, vfs_normalize(NULL, "etc", out, sizeof(out)));
}

// ---------------------------------------------------------------------------
// Монтирование
// ---------------------------------------------------------------------------

// Пустой бэкенд нужен только для проверки таблицы: операции не вызываются.
static int dummy_stat(void* ctx, const char* path, vfs_stat_t* out)
{
    (void)ctx;
    (void)path;
    if (out != NULL) {
        out->size = 0;
        out->type = VFS_TYPE_DIR;
    }
    return VFS_OK;
}

void test_mount_root_and_tmp(void)
{
    vfs_init();
    vfs_backend_t be;
    memset(&be, 0, sizeof(be));
    be.stat = dummy_stat;

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/", &be));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/tmp", &be));

    // Пока ничего не сломали: "/" и "/tmp" - разные точки.
    TEST_ASSERT_TRUE(vfs_mounted());
}

// Заглушка, которая запоминает, куда именно пришёл запрос: так проверяется
// выбор самой длинной точки и то, что внутрь неё уходит путь без её префикса.
static const char* s_seen_ctx;
static char s_seen_path[VFS_PATH_MAX];

static int mark_stat(void* ctx, const char* path, vfs_stat_t* out)
{
    s_seen_ctx = (const char*)ctx;
    snprintf(s_seen_path, sizeof(s_seen_path), "%s", path);
    if (out != NULL) {
        out->size = 0;
        out->type = VFS_TYPE_DIR;
    }
    return VFS_OK;
}

void test_mount_picks_longest_prefix(void)
{
    vfs_init();
    vfs_backend_t root;
    vfs_backend_t tmp;
    memset(&root, 0, sizeof(root));
    memset(&tmp, 0, sizeof(tmp));
    root.ctx = (void*)"root";
    root.stat = mark_stat;
    tmp.ctx = (void*)"tmp";
    tmp.stat = mark_stat;

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/", &root));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/tmp", &tmp));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/etc/x", NULL));
    TEST_ASSERT_EQUAL_STRING("root", s_seen_ctx);
    TEST_ASSERT_EQUAL_STRING("/etc/x", s_seen_path);

    // Из вложенных точек берётся самая длинная, а префикс отрезается.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/tmp/y", NULL));
    TEST_ASSERT_EQUAL_STRING("tmp", s_seen_ctx);
    TEST_ASSERT_EQUAL_STRING("/y", s_seen_path);

    // Сама точка: путь внутри неё становится "/", а не пустой строкой.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/tmp", NULL));
    TEST_ASSERT_EQUAL_STRING("tmp", s_seen_ctx);
    TEST_ASSERT_EQUAL_STRING("/", s_seen_path);

    // Похожее имя - не тот же префикс: "/tmpfile" живёт в корне.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/tmpfile", NULL));
    TEST_ASSERT_EQUAL_STRING("root", s_seen_ctx);

    // Войдя в точку, относительный путь считается уже внутри неё.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_chdir("/tmp"));
    TEST_ASSERT_EQUAL_STRING("/tmp", vfs_cwd());
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("z", NULL));
    TEST_ASSERT_EQUAL_STRING("tmp", s_seen_ctx);
    TEST_ASSERT_EQUAL_STRING("/z", s_seen_path);
}

void test_mount_rejects_duplicates(void)
{
    vfs_init();
    vfs_backend_t be;
    memset(&be, 0, sizeof(be));
    be.stat = dummy_stat;

    // Вложение разрешено: "/tmp" внутри "/" - это и есть точка монтирования
    // внутри корня, поиск берёт самый длинный подходящий префикс.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/", &be));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_EXISTS, vfs_mount("/", &be));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/tmp", &be));

    vfs_init();
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/tmp", &be));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/tmp/x", &be));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_EXISTS, vfs_mount("/tmp", &be));
    // Сосед с тем же началом - не пересечение.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mount("/tmp2", &be));
}

void test_mount_requires_absolute(void)
{
    vfs_init();
    vfs_backend_t be;
    memset(&be, 0, sizeof(be));
    be.stat = dummy_stat;
    TEST_ASSERT_EQUAL_INT(VFS_ERR_ARG, vfs_mount("tmp", &be));
}

void test_operations_without_mount(void)
{
    vfs_init();
    TEST_ASSERT_FALSE(vfs_mounted());
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NO_MOUNT, vfs_stat("/x", NULL));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NO_MOUNT, vfs_chdir("/"));
}

// ---------------------------------------------------------------------------
// Пути и текущий каталог поверх настоящей ФС
// ---------------------------------------------------------------------------

void test_stat_and_mkdir(void)
{
    mount_fresh();

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/", &st));
    TEST_ASSERT_EQUAL_UINT16(VFS_TYPE_DIR, st.type);

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/etc"));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_EXISTS, vfs_mkdir("/etc"));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/etc", &st));
    TEST_ASSERT_EQUAL_UINT16(VFS_TYPE_DIR, st.type);

    // Создание файла - только через open с флагом: mkdir для каталога.
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_FOUND, vfs_stat("/etc/rc", NULL));
}

void test_chdir_and_relative_paths(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/etc"));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/etc/rc.d"));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_chdir("/etc"));
    TEST_ASSERT_EQUAL_STRING("/etc", vfs_cwd());

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("rc.d2"));
    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/etc/rc.d2", &st));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_chdir("rc.d"));
    TEST_ASSERT_EQUAL_STRING("/etc/rc.d", vfs_cwd());

    // ".." работает через VFS, хотя BFS его не умеет.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_chdir(".."));
    TEST_ASSERT_EQUAL_STRING("/etc", vfs_cwd());
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_chdir("../.."));
    TEST_ASSERT_EQUAL_STRING("/", vfs_cwd());
}

void test_chdir_into_file_fails(void)
{
    mount_fresh();
    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_CREATE, &fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_DIR, vfs_chdir("/f"));
    TEST_ASSERT_EQUAL_STRING("/", vfs_cwd());

    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_FOUND, vfs_chdir("/nope"));
    TEST_ASSERT_EQUAL_STRING("/", vfs_cwd());
}

// ---------------------------------------------------------------------------
// Открытые файлы
// ---------------------------------------------------------------------------

void test_open_create_write_read(void)
{
    mount_fresh();

    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/hello", VFS_O_CREATE | VFS_O_WRONLY, &fd));

    const char text[] = "привет";
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_write(fd, text, sizeof(text) - 1, &done));
    TEST_ASSERT_EQUAL_UINT32(sizeof(text) - 1, done);
    TEST_ASSERT_EQUAL_UINT32(sizeof(text) - 1, vfs_tell(fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/hello", VFS_O_RDONLY, &fd));
    char buf[16];
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_read(fd, buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_UINT32(sizeof(text) - 1, done);
    TEST_ASSERT_EQUAL_STRING(text, buf);

    // Второй read на конце файла даёт ноль - это и есть конец.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_read(fd, buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_UINT32(0, done);
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));
}

void test_open_without_create_fails(void)
{
    mount_fresh();
    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_FOUND, vfs_open("/nope", VFS_O_RDONLY, &fd));
}

void test_open_directory_fails(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/etc"));
    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_ERR_IS_DIR, vfs_open("/etc", VFS_O_RDONLY, &fd));
}

void test_trunc_and_append(void)
{
    mount_fresh();

    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_CREATE, &fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_write(fd, "abcdef", 6, NULL));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    // TRUNC обнуляет: то, что нужно для "> файл" в шелле.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_TRUNC, &fd));
    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/f", &st));
    TEST_ASSERT_EQUAL_UINT32(0, st.size);
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_write(fd, "x", 1, NULL));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    // APPEND встаёт в конец, а не в ноль.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_APPEND, &fd));
    TEST_ASSERT_EQUAL_UINT32(1, vfs_tell(fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_write(fd, "y", 1, NULL));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/f", &st));
    TEST_ASSERT_EQUAL_UINT32(2, st.size);
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_RDONLY, &fd));
    char buf[4] = {0};
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_read(fd, buf, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL_MEMORY("xy", buf, 2);
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));
}

// Усечение отдельной операцией: оболочке оно нужно отложенным, уже после того
// как файл открыт на запись (`> f` не должен обнулять файл раньше времени).
void test_truncate_path(void)
{
    mount_fresh();

    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_CREATE, &fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_write(fd, "abcdef", 6, NULL));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    // Файл открыт, но не тронут: усечение отдельной командой работает.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_WRONLY, &fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_truncate("/f", 0));
    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/f", &st));
    TEST_ASSERT_EQUAL_UINT32(0, st.size);
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_write(fd, "z", 1, NULL));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/f", &st));
    TEST_ASSERT_EQUAL_UINT32(1, st.size);

    // Каталог обрезать нельзя, а несуществующий файл - ошибка, а не удача.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/d"));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_IS_DIR, vfs_truncate("/d", 0));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_FOUND, vfs_truncate("/nope", 0));
}

void test_seek_and_read_at(void)
{
    mount_fresh();

    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_CREATE, &fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_write(fd, "0123456789", 10, NULL));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_seek(fd, 3));
    TEST_ASSERT_EQUAL_UINT32(3, vfs_tell(fd));

    char buf[4] = {0};
    uint32_t done = 0;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_read(fd, buf, 3, &done));
    TEST_ASSERT_EQUAL_UINT32(3, done);
    TEST_ASSERT_EQUAL_MEMORY("345", buf, 3);
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));
}

void test_fd_table_exhausts_and_reuses(void)
{
    mount_fresh();

    int fds[VFS_OPEN_MAX];
    for (size_t i = 0; i < VFS_OPEN_MAX; ++i) {
        char path[16];
        snprintf(path, sizeof(path), "/f%u", (unsigned)i);
        TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open(path, VFS_O_CREATE, &fds[i]));
    }

    int extra = -1;
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NO_FD, vfs_open("/more", VFS_O_CREATE, &extra));

    // Закрыли один - слот снова свободен. Номер закрытого файла запоминаем
    // отдельно: open вправе вернуть тот же номер, и тогда проверять «закрытый
    // fd невалиден» уже не на чем.
    const int closed = fds[3];
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(closed));

    // Закрыть дважды или обратиться к чужому номеру - отказ.
    TEST_ASSERT_EQUAL_INT(VFS_ERR_BAD_FD, vfs_close(closed));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_BAD_FD, vfs_close(-1));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_BAD_FD, vfs_close((int)VFS_OPEN_MAX));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_BAD_FD, vfs_read(closed, &extra, 1, NULL));

    // Освободившийся слот переиспользуется - иначе таблица текла бы.
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/more", VFS_O_CREATE, &extra));
    TEST_ASSERT_EQUAL_INT(closed, extra);
}

// ---------------------------------------------------------------------------
// Каталоги, обход, переименование
// ---------------------------------------------------------------------------

void test_readdir_walks_entries(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/etc"));
    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/etc/rc", VFS_O_CREATE, &fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_write(fd, "abc", 3, NULL));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    char name[VFS_NAME_MAX + 1];
    vfs_stat_t st;

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_readdir("/", 0, name, &st));
    TEST_ASSERT_EQUAL_STRING("etc", name);
    TEST_ASSERT_EQUAL_UINT16(VFS_TYPE_DIR, st.type);

    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_FOUND, vfs_readdir("/", 1, name, &st));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_readdir("/etc", 0, name, &st));
    TEST_ASSERT_EQUAL_STRING("rc", name);
    TEST_ASSERT_EQUAL_UINT16(VFS_TYPE_FILE, st.type);
    TEST_ASSERT_EQUAL_UINT32(3, st.size);

    // Обход файла - не «пусто», а ошибка: это не каталог.
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_DIR, vfs_readdir("/etc/rc", 0, name, &st));
}

void test_rename_and_remove(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/a"));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/b"));

    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/a/f", VFS_O_CREATE, &fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_rename("/a/f", "/b/g"));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_FOUND, vfs_stat("/a/f", NULL));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/b/g", NULL));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_unlink("/b/g"));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_FOUND, vfs_stat("/b/g", NULL));

    // rmdir непустого каталога и unlink каталога - разными кодами: оболочке
    // по ним нужно решать, что писать в ответ.
    TEST_ASSERT_EQUAL_INT(VFS_ERR_IS_DIR, vfs_unlink("/a"));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/a/sub"));
    TEST_ASSERT_EQUAL_INT(VFS_ERR_NOT_EMPTY, vfs_rmdir("/a"));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_rmdir("/a/sub"));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_rmdir("/a"));
}

// Пока в таблице одна точка, проверить отказ при переименовании через границу
// нельзя. Проверяем то, что можно: путь из одной точки в ту же точку через
// ".." остаётся внутри неё и работает.
void test_rename_through_dotdot(void)
{
    mount_fresh();
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_mkdir("/d"));
    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_CREATE, &fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_chdir("/d"));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_rename("../f", "f2"));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/d/f2", NULL));
}

// Время идёт снаружи, а не из ФС: vfs_set_now обходит точки монтирования и
// передаёт значение в бэкенд. Проверяем, что оно доходит до inode и видно в
// stat и readdir - именно по этим двум путям `ls -l` и берёт дату.
void test_time_propagates_to_stat_and_readdir(void)
{
    mount_fresh();
    vfs_set_now(1794076923u);

    int fd = -1;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_open("/f", VFS_O_CREATE, &fd));
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_close(fd));

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/f", &st));
    TEST_ASSERT_EQUAL_UINT32(1794076923u, st.mtime);

    char name[VFS_NAME_MAX + 1];
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_readdir("/", 0, name, &st));
    TEST_ASSERT_EQUAL_STRING("f", name);
    TEST_ASSERT_EQUAL_UINT32(1794076923u, st.mtime);

    // Сброс в «время неизвестно» не должен стирать уже записанную метку.
    vfs_set_now(0);
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_stat("/f", &st));
    TEST_ASSERT_EQUAL_UINT32(1794076923u, st.mtime);
}

void test_usage(void)
{
    mount_fresh();
    uint32_t used = 0;
    uint32_t total = 0;
    TEST_ASSERT_EQUAL_INT(VFS_OK, vfs_usage("/", &used, &total));
    // VFS отдаёт байты, а не блоки: размер блока - дело конкретной ФС.
    TEST_ASSERT_EQUAL_UINT32(TEST_BLOCKS * BFS_BLOCK_SIZE, total);
    TEST_ASSERT_TRUE(used > 0);
    TEST_ASSERT_TRUE(used < total);
}

// ---------------------------------------------------------------------------
// Тексты ошибок
// ---------------------------------------------------------------------------

void test_strerror_is_never_empty(void)
{
    TEST_ASSERT_TRUE(strlen(vfs_strerror(VFS_OK)) > 0);
    TEST_ASSERT_TRUE(strlen(vfs_strerror(VFS_ERR_NOT_FOUND)) > 0);
    TEST_ASSERT_TRUE(strlen(vfs_strerror(VFS_ERR_NO_MOUNT)) > 0);
    TEST_ASSERT_TRUE(strlen(vfs_strerror(-999)) > 0);
}

// ---------------------------------------------------------------------------

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_normalize_absolute);
    RUN_TEST(test_normalize_relative);
    RUN_TEST(test_normalize_dotdot);
    RUN_TEST(test_normalize_dots_in_names);
    RUN_TEST(test_normalize_rejects_bad);
    RUN_TEST(test_normalize_relative_needs_cwd);

    RUN_TEST(test_mount_root_and_tmp);
    RUN_TEST(test_mount_picks_longest_prefix);
    RUN_TEST(test_mount_rejects_duplicates);
    RUN_TEST(test_mount_requires_absolute);
    RUN_TEST(test_operations_without_mount);

    RUN_TEST(test_stat_and_mkdir);
    RUN_TEST(test_chdir_and_relative_paths);
    RUN_TEST(test_chdir_into_file_fails);

    RUN_TEST(test_open_create_write_read);
    RUN_TEST(test_open_without_create_fails);
    RUN_TEST(test_open_directory_fails);
    RUN_TEST(test_trunc_and_append);
    RUN_TEST(test_truncate_path);
    RUN_TEST(test_seek_and_read_at);
    RUN_TEST(test_fd_table_exhausts_and_reuses);

    RUN_TEST(test_readdir_walks_entries);
    RUN_TEST(test_rename_and_remove);
    RUN_TEST(test_rename_through_dotdot);
    RUN_TEST(test_time_propagates_to_stat_and_readdir);
    RUN_TEST(test_usage);

    RUN_TEST(test_strerror_is_never_empty);

    return UNITY_END();
}
