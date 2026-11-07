// VFS: канонизация пути, точки монтирования и таблица открытых файлов.
// Устройство и причины - в vfs.h.

#include "vfs.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Таблицы
//
// Статические, а не из кучи: слотов мало, размер известен, а память под них
// нужна с первой секунды после старта - до того, как станет ясно, что куча
// вообще работает. Ошибка «забыли закрыть файл» при этом видна сразу: слоты
// кончились.
// ---------------------------------------------------------------------------

typedef struct {
    bool used;
    char prefix[VFS_PATH_MAX];
    vfs_backend_t backend;
} mount_entry_t;

typedef struct {
    bool used;
    int mount;
    char path[VFS_PATH_MAX];  // путь внутри точки монтирования, канонизированный
    uint32_t offset;
} open_entry_t;

static mount_entry_t s_mounts[VFS_MOUNT_MAX];
static open_entry_t s_open[VFS_OPEN_MAX];
static char s_cwd[VFS_PATH_MAX] = "/";

void vfs_init(void)
{
    memset(s_mounts, 0, sizeof(s_mounts));
    memset(s_open, 0, sizeof(s_open));
    strcpy(s_cwd, "/");
}

bool vfs_mounted(void)
{
    for (size_t i = 0; i < VFS_MOUNT_MAX; ++i) {
        if (s_mounts[i].used) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Канонизация пути
// ---------------------------------------------------------------------------

int vfs_normalize(const char* cwd, const char* path, char* out, size_t out_size)
{
    if (path == NULL || out == NULL || out_size == 0) {
        return VFS_ERR_ARG;
    }
    // Пустой путь не означает «текущий каталог»: он означает, что имя забыли
    // передать. Молча подставить cwd значило бы открыть не тот файл.
    if (path[0] == '\0') {
        return VFS_ERR_NAME;
    }

    // Сначала склеиваем путь с текущим каталогом, потом разбираем компоненты.
    // Один буфер на 128 байт на стеке - это дешевле, чем два прохода по
    // исходной строке с подсчётом длин.
    char work[VFS_PATH_MAX];
    size_t n = 0;

    if (path[0] != '/') {
        if (cwd == NULL) {
            return VFS_ERR_ARG;
        }
        const size_t clen = strlen(cwd);
        if (clen >= sizeof(work)) {
            return VFS_ERR_NAME;
        }
        memcpy(work, cwd, clen);
        n = clen;
        if (n == 0 || work[n - 1] != '/') {
            if (n + 1 >= sizeof(work)) {
                return VFS_ERR_NAME;
            }
            work[n++] = '/';
        }
    }

    for (const char* p = path; *p != '\0'; ++p) {
        if (n + 1 >= sizeof(work)) {
            return VFS_ERR_NAME;
        }
        work[n++] = *p;
    }
    work[n] = '\0';

    // Компоненты собираются заново, поэтому «..» обрабатывается без стека:
    // достаточно отрезать от результата последний компонент.
    char result[VFS_PATH_MAX];
    size_t rlen = 0;

    const char* p = work;
    while (*p != '\0') {
        while (*p == '/') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const char* const start = p;
        while (*p != '\0' && *p != '/') {
            ++p;
        }
        const size_t clen = (size_t)(p - start);

        if (clen == 1 && start[0] == '.') {
            continue;
        }
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            // Выше корня некуда: "/.." - это "/". Так же ведут себя
            // POSIX-системы, и это единственное правило, которое не требует
            // хранить ссылку на родителя.
            while (rlen > 0 && result[rlen - 1] != '/') {
                --rlen;
            }
            if (rlen > 0) {
                --rlen;
            }
            continue;
        }

        if (rlen + clen + 2 > sizeof(result)) {
            return VFS_ERR_NAME;
        }
        result[rlen++] = '/';
        memcpy(result + rlen, start, clen);
        rlen += clen;
    }

    // Корень - отдельный случай: пустой результат означает "/", а не "".
    const size_t need = (rlen == 0) ? 2u : rlen + 1u;
    if (need > out_size) {
        return VFS_ERR_NAME;
    }
    if (rlen == 0) {
        out[0] = '/';
        out[1] = '\0';
        return VFS_OK;
    }
    memcpy(out, result, rlen);
    out[rlen] = '\0';
    return VFS_OK;
}

int vfs_resolve(const char* path, char* out, size_t out_size)
{
    return vfs_normalize(s_cwd, path, out, out_size);
}

// ---------------------------------------------------------------------------
// Точки монтирования
// ---------------------------------------------------------------------------

// Совпадают ли два канонических префикса. Вложенность разрешена и нужна:
// "/tmp" внутри "/" - это и есть точка монтирования внутри корня, а поиск
// берёт самую длинную подходящую. Запрещена только вторая точка с тем же
// префиксом: непонятно, чья она, и «смонтировали поверх» - это уже
// размонтирование, которого в интерфейсе нет.
static bool same_prefix(const char* a, const char* b)
{
    return strcmp(a, b) == 0;
}

int vfs_mount(const char* prefix, const vfs_backend_t* backend)
{
    if (prefix == NULL || backend == NULL || backend->stat == NULL) {
        return VFS_ERR_ARG;
    }
    if (prefix[0] != '/') {
        return VFS_ERR_ARG;
    }

    char canon[VFS_PATH_MAX];
    int rc = vfs_normalize("/", prefix, canon, sizeof(canon));
    if (rc != VFS_OK) {
        return rc;
    }

    for (size_t i = 0; i < VFS_MOUNT_MAX; ++i) {
        if (s_mounts[i].used && same_prefix(s_mounts[i].prefix, canon)) {
            return VFS_ERR_EXISTS;
        }
    }

    for (size_t i = 0; i < VFS_MOUNT_MAX; ++i) {
        if (s_mounts[i].used) {
            continue;
        }
        s_mounts[i].used = true;
        strcpy(s_mounts[i].prefix, canon);
        s_mounts[i].backend = *backend;
        return VFS_OK;
    }
    return VFS_ERR_NO_SPACE;
}

// Найти точку для канонического пути. Возвращает индекс в таблице или -1, а в
// inside - путь внутри точки (всегда начинается с '/').
static int find_mount(const char* abs, const char** inside)
{
    int best = -1;
    size_t best_len = 0;

    for (size_t i = 0; i < VFS_MOUNT_MAX; ++i) {
        if (!s_mounts[i].used) {
            continue;
        }
        const char* const pre = s_mounts[i].prefix;
        const size_t plen = strlen(pre);
        if (strncmp(abs, pre, plen) != 0) {
            continue;
        }
        // "/tmp" не должен совпадать с "/tmpfile": после префикса обязан идти
        // конец строки или слэш.
        if (plen > 1 && abs[plen] != '\0' && abs[plen] != '/') {
            continue;
        }
        if (best < 0 || plen > best_len) {
            best = (int)i;
            best_len = plen;
        }
    }

    if (best < 0) {
        return -1;
    }

    // У корня префикс и есть весь путь: отрезать "/" нельзя, иначе путь
    // внутри точки потеряет ведущий слэш и бэкенд его не поймёт.
    if (best_len == 1 && s_mounts[best].prefix[0] == '/') {
        *inside = abs;
    } else {
        const char* in = abs + best_len;
        *inside = (*in == '\0') ? "/" : in;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Общая часть операций над путём
// ---------------------------------------------------------------------------

// Канонизировать путь и найти точку. Возвращает индекс точки, в in - путь
// внутри неё.
static int locate(const char* path, char* abs, size_t abs_size, const char** in)
{
    int rc = vfs_resolve(path, abs, abs_size);
    if (rc != VFS_OK) {
        return rc;
    }
    const int m = find_mount(abs, in);
    return (m < 0) ? VFS_ERR_NO_MOUNT : m;
}

int vfs_chdir(const char* path)
{
    char abs[VFS_PATH_MAX];
    const char* in = NULL;
    const int m = locate(path, abs, sizeof(abs), &in);
    if (m < 0) {
        return m;
    }

    vfs_stat_t st;
    const int rc = s_mounts[m].backend.stat(s_mounts[m].backend.ctx, in, &st);
    if (rc != VFS_OK) {
        return rc;
    }
    if (st.type != VFS_TYPE_DIR) {
        return VFS_ERR_NOT_DIR;
    }

    strcpy(s_cwd, abs);
    return VFS_OK;
}

const char* vfs_cwd(void)
{
    return s_cwd;
}

int vfs_getcwd(char* out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return VFS_ERR_ARG;
    }
    const size_t n = strlen(s_cwd);
    if (n + 1 > out_size) {
        return VFS_ERR_NAME;
    }
    memcpy(out, s_cwd, n + 1);
    return VFS_OK;
}

int vfs_stat(const char* path, vfs_stat_t* out)
{
    char abs[VFS_PATH_MAX];
    const char* in = NULL;
    const int m = locate(path, abs, sizeof(abs), &in);
    if (m < 0) {
        return m;
    }
    return s_mounts[m].backend.stat(s_mounts[m].backend.ctx, in, out);
}

int vfs_mkdir(const char* path)
{
    char abs[VFS_PATH_MAX];
    const char* in = NULL;
    const int m = locate(path, abs, sizeof(abs), &in);
    if (m < 0) {
        return m;
    }
    if (s_mounts[m].backend.mkdir == NULL) {
        return VFS_ERR_ARG;
    }
    return s_mounts[m].backend.mkdir(s_mounts[m].backend.ctx, in);
}

int vfs_unlink(const char* path)
{
    char abs[VFS_PATH_MAX];
    const char* in = NULL;
    const int m = locate(path, abs, sizeof(abs), &in);
    if (m < 0) {
        return m;
    }
    if (s_mounts[m].backend.unlink == NULL) {
        return VFS_ERR_ARG;
    }
    return s_mounts[m].backend.unlink(s_mounts[m].backend.ctx, in);
}

int vfs_rmdir(const char* path)
{
    char abs[VFS_PATH_MAX];
    const char* in = NULL;
    const int m = locate(path, abs, sizeof(abs), &in);
    if (m < 0) {
        return m;
    }
    if (s_mounts[m].backend.rmdir == NULL) {
        return VFS_ERR_ARG;
    }
    return s_mounts[m].backend.rmdir(s_mounts[m].backend.ctx, in);
}

// Переименование через границу точек монтирования не поддерживается: это уже
// копирование с удалением, и решать, что делать при нехватке места на второй
// точке, должен тот, кто просил. Пока такой просьбы нет - честный отказ.
int vfs_rename(const char* from, const char* to)
{
    char from_abs[VFS_PATH_MAX];
    char to_abs[VFS_PATH_MAX];
    const char* from_in = NULL;
    const char* to_in = NULL;

    const int m1 = locate(from, from_abs, sizeof(from_abs), &from_in);
    if (m1 < 0) {
        return m1;
    }
    const int m2 = locate(to, to_abs, sizeof(to_abs), &to_in);
    if (m2 < 0) {
        return m2;
    }
    if (m1 != m2) {
        return VFS_ERR_ARG;
    }
    if (s_mounts[m1].backend.rename == NULL) {
        return VFS_ERR_ARG;
    }
    return s_mounts[m1].backend.rename(s_mounts[m1].backend.ctx, from_in, to_in);
}

int vfs_truncate(const char* path, uint32_t size)
{
    char abs[VFS_PATH_MAX];
    const char* in = NULL;
    const int m = locate(path, abs, sizeof(abs), &in);
    if (m < 0) {
        return m;
    }

    vfs_backend_t* const be = &s_mounts[m].backend;
    if (be->truncate == NULL) {
        return VFS_ERR_ARG;
    }

    // Каталог «обрезать» нельзя: у него нет длины, и молчаливая удача здесь
    // означала бы, что вызывающий считает операцию выполненной.
    vfs_stat_t st;
    const int rc = be->stat(be->ctx, in, &st);
    if (rc != VFS_OK) {
        return rc;
    }
    if (st.type != VFS_TYPE_FILE) {
        return VFS_ERR_IS_DIR;
    }
    return be->truncate(be->ctx, in, size);
}

int vfs_readdir(const char* path, uint32_t index, char* name, vfs_stat_t* out)
{
    char abs[VFS_PATH_MAX];
    const char* in = NULL;
    const int m = locate(path, abs, sizeof(abs), &in);
    if (m < 0) {
        return m;
    }
    if (s_mounts[m].backend.readdir == NULL) {
        return VFS_ERR_ARG;
    }
    if (name != NULL) {
        name[0] = '\0';
    }
    return s_mounts[m].backend.readdir(s_mounts[m].backend.ctx, in, index, name, out);
}

int vfs_usage(const char* path, uint32_t* used, uint32_t* total)
{
    char abs[VFS_PATH_MAX];
    const char* in = NULL;
    const int m = locate(path, abs, sizeof(abs), &in);
    if (m < 0) {
        return m;
    }
    (void)in;
    if (s_mounts[m].backend.usage == NULL) {
        return VFS_ERR_ARG;
    }
    return s_mounts[m].backend.usage(s_mounts[m].backend.ctx, used, total);
}

void vfs_set_now(uint32_t unix_secs)
{
    for (size_t i = 0; i < VFS_MOUNT_MAX; ++i) {
        if (s_mounts[i].used && s_mounts[i].backend.set_now != NULL) {
            s_mounts[i].backend.set_now(s_mounts[i].backend.ctx, unix_secs);
        }
    }
}

// ---------------------------------------------------------------------------
// Открытые файлы
// ---------------------------------------------------------------------------

int vfs_open(const char* path, uint8_t flags, int* fd)
{
    if (fd == NULL) {
        return VFS_ERR_ARG;
    }

    char abs[VFS_PATH_MAX];
    const char* in = NULL;
    const int m = locate(path, abs, sizeof(abs), &in);
    if (m < 0) {
        return m;
    }

    vfs_backend_t* const be = &s_mounts[m].backend;

    // Сначала выясняем, что там: открыть каталог как файл нельзя, а «создать
    // поверх каталога» - это потеря дерева.
    vfs_stat_t st;
    int rc = be->stat(be->ctx, in, &st);
    if (rc == VFS_ERR_NOT_FOUND) {
        if ((flags & VFS_O_CREATE) == 0) {
            return rc;
        }
        if (be->create == NULL) {
            return VFS_ERR_ARG;
        }
        rc = be->create(be->ctx, in);
        if (rc != VFS_OK) {
            return rc;
        }
        st.size = 0;
        st.type = VFS_TYPE_FILE;
    } else if (rc != VFS_OK) {
        return rc;
    }

    if (st.type != VFS_TYPE_FILE) {
        return VFS_ERR_IS_DIR;
    }

    if ((flags & VFS_O_TRUNC) != 0 && st.size != 0) {
        if (be->truncate == NULL) {
            return VFS_ERR_ARG;
        }
        rc = be->truncate(be->ctx, in, 0);
        if (rc != VFS_OK) {
            return rc;
        }
        st.size = 0;
    }

    int slot = -1;
    for (size_t i = 0; i < VFS_OPEN_MAX; ++i) {
        if (!s_open[i].used) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return VFS_ERR_NO_FD;
    }

    open_entry_t* const e = &s_open[slot];
    e->used = true;
    e->mount = m;
    strcpy(e->path, in);
    e->offset = ((flags & VFS_O_APPEND) != 0) ? st.size : 0;
    *fd = slot;
    return VFS_OK;
}

static open_entry_t* open_entry(int fd)
{
    if (fd < 0 || (size_t)fd >= VFS_OPEN_MAX || !s_open[fd].used) {
        return NULL;
    }
    return &s_open[fd];
}

int vfs_read(int fd, void* dst, uint32_t len, uint32_t* done)
{
    if (done != NULL) {
        *done = 0;
    }
    open_entry_t* const e = open_entry(fd);
    if (e == NULL) {
        return VFS_ERR_BAD_FD;
    }
    vfs_backend_t* const be = &s_mounts[e->mount].backend;
    if (be->read == NULL) {
        return VFS_ERR_ARG;
    }

    uint32_t got = 0;
    const int rc = be->read(be->ctx, e->path, e->offset, dst, len, &got);
    if (rc != VFS_OK) {
        return rc;
    }
    // Позиция сдвигается только на прочитанное: на конце файла вернётся 0, и
    // следующий вызов снова спросит то же место - это и есть признак конца,
    // отдельного флага «EOF» в интерфейсе нет.
    e->offset += got;
    if (done != NULL) {
        *done = got;
    }
    return VFS_OK;
}

int vfs_write(int fd, const void* src, uint32_t len, uint32_t* done)
{
    if (done != NULL) {
        *done = 0;
    }
    open_entry_t* const e = open_entry(fd);
    if (e == NULL) {
        return VFS_ERR_BAD_FD;
    }
    vfs_backend_t* const be = &s_mounts[e->mount].backend;
    if (be->write == NULL) {
        return VFS_ERR_ARG;
    }

    uint32_t put = 0;
    const int rc = be->write(be->ctx, e->path, e->offset, src, len, &put);
    if (rc != VFS_OK) {
        return rc;
    }
    e->offset += put;
    if (done != NULL) {
        *done = put;
    }
    return VFS_OK;
}

int vfs_seek(int fd, uint32_t offset)
{
    open_entry_t* const e = open_entry(fd);
    if (e == NULL) {
        return VFS_ERR_BAD_FD;
    }
    // За конец файла пускать можно: это разреженный файл, а не ошибка. ФС
    // сама решит, что делать (BFS читает дырку нулями).
    e->offset = offset;
    return VFS_OK;
}

uint32_t vfs_tell(int fd)
{
    const open_entry_t* const e = open_entry(fd);
    return (e == NULL) ? 0u : e->offset;
}

int vfs_close(int fd)
{
    open_entry_t* const e = open_entry(fd);
    if (e == NULL) {
        return VFS_ERR_BAD_FD;
    }
    // Отдельного sync нет: BFS коммитит суперблок в конце каждой операции,
    // поэтому закрытие ничего не записывает. Когда появится ФС с отложенной
    // записью, сюда добавится вызов бэкенда.
    memset(e, 0, sizeof(*e));
    return VFS_OK;
}

// ---------------------------------------------------------------------------
// Тексты ошибок
// ---------------------------------------------------------------------------

const char* vfs_strerror(int rc)
{
    switch (rc) {
    case VFS_OK: return "ok";
    case VFS_ERR_IO: return "отказ устройства";
    case VFS_ERR_FORMAT: return "раздел не размечен";
    case VFS_ERR_NO_SPACE: return "нет места";
    case VFS_ERR_NOT_FOUND: return "не найдено";
    case VFS_ERR_EXISTS: return "уже есть";
    case VFS_ERR_NOT_DIR: return "не каталог";
    case VFS_ERR_IS_DIR: return "это каталог";
    case VFS_ERR_NOT_EMPTY: return "каталог не пуст";
    case VFS_ERR_NAME: return "негодное имя или путь";
    case VFS_ERR_ARG: return "негодный аргумент";
    case VFS_ERR_NO_MOUNT: return "сюда ничего не смонтировано";
    case VFS_ERR_NO_FD: return "кончились открытые файлы";
    case VFS_ERR_BAD_FD: return "файл не открыт";
    default: return "неизвестная ошибка";
    }
}
