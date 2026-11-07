// Адаптер: BFS как бэкенд VFS.
//
// Зачем отдельный файл, а не «пусть BFS сам реализует vfs_backend_t». Тогда
// bfs.h пришлось бы включить vfs.h, то есть ФС узнала бы про пространство
// имён, текущий каталог и монтирование - ровно то, чего она знать не должна:
// на маке она проверяется одна, без VFS, и остаётся готовой к тому, чтобы
// когда-нибудь лечь под другую ОС без переделки.
//
// Цена - этот файл: перевод кодов и типов в обе стороны, по строке на
// операцию. Он того стоит: без него граница между «файл на диске» и «путь в
// системе» проходила бы через случайное место, и первая же вторая ФС
// потребовала бы ломать интерфейс.

#include "vfs_bfs.h"

#include <string.h>

#include "bfs.h"

// Коды BFS и VFS совпадают по смыслу, но не по контракту: это разные
// перечисления, объявленные в разных заголовках, и разъехаться они могут
// (VFS добавит свой код - у BFS его не будет). Поэтому перевод явный: если
// появится новый код, компилятор заставит его увидеть.
static int map_error(int rc)
{
    switch (rc) {
    case BFS_OK: return VFS_OK;
    case BFS_ERR_IO: return VFS_ERR_IO;
    case BFS_ERR_FORMAT: return VFS_ERR_FORMAT;
    case BFS_ERR_NO_SPACE: return VFS_ERR_NO_SPACE;
    case BFS_ERR_NOT_FOUND: return VFS_ERR_NOT_FOUND;
    case BFS_ERR_EXISTS: return VFS_ERR_EXISTS;
    case BFS_ERR_NOT_DIR: return VFS_ERR_NOT_DIR;
    case BFS_ERR_IS_DIR: return VFS_ERR_IS_DIR;
    case BFS_ERR_NOT_EMPTY: return VFS_ERR_NOT_EMPTY;
    case BFS_ERR_NAME: return VFS_ERR_NAME;
    case BFS_ERR_BROKEN: return VFS_ERR_IO;  // для вызывающего это отказ устройства
    default: return VFS_ERR_ARG;
    }
}

static uint16_t map_type(uint16_t t)
{
    return (t == BFS_TYPE_DIR) ? (uint16_t)VFS_TYPE_DIR : (uint16_t)VFS_TYPE_FILE;
}

static int bfs_stat_op(void* ctx, const char* path, vfs_stat_t* out)
{
    bfs_stat_t st;
    const int rc = bfs_stat((bfs_t*)ctx, path, &st);
    if (rc != BFS_OK) {
        return map_error(rc);
    }
    if (out != NULL) {
        out->size = st.size;
        out->mtime = st.mtime;
        out->type = map_type(st.type);
    }
    return VFS_OK;
}

static int bfs_create_op(void* ctx, const char* path)
{
    return map_error(bfs_create((bfs_t*)ctx, path));
}

static int bfs_mkdir_op(void* ctx, const char* path)
{
    return map_error(bfs_mkdir((bfs_t*)ctx, path));
}

static int bfs_unlink_op(void* ctx, const char* path)
{
    return map_error(bfs_unlink((bfs_t*)ctx, path));
}

static int bfs_rmdir_op(void* ctx, const char* path)
{
    return map_error(bfs_rmdir((bfs_t*)ctx, path));
}

static int bfs_rename_op(void* ctx, const char* from, const char* to)
{
    return map_error(bfs_rename((bfs_t*)ctx, from, to));
}

static int bfs_readdir_op(void* ctx, const char* path, uint32_t index, char* name,
                          vfs_stat_t* out)
{
    bfs_stat_t st;
    const int rc = bfs_readdir((bfs_t*)ctx, path, index, name, &st);
    if (rc != BFS_OK) {
        return map_error(rc);
    }
    if (out != NULL) {
        out->type = map_type(st.type);
        out->size = st.size;
        out->mtime = st.mtime;
    }
    return VFS_OK;
}

static int bfs_read_op(void* ctx, const char* path, uint32_t offset, void* dst, uint32_t len,
                       uint32_t* done)
{
    return map_error(bfs_read((bfs_t*)ctx, path, offset, dst, len, done));
}

static int bfs_write_op(void* ctx, const char* path, uint32_t offset, const void* src,
                        uint32_t len, uint32_t* done)
{
    return map_error(bfs_write((bfs_t*)ctx, path, offset, src, len, done));
}

static int bfs_truncate_op(void* ctx, const char* path, uint32_t size)
{
    return map_error(bfs_truncate((bfs_t*)ctx, path, size));
}

static int bfs_usage_op(void* ctx, uint32_t* used, uint32_t* total)
{
    uint32_t blocks_used = 0;
    uint32_t blocks_total = 0;
    const int rc = bfs_usage((const bfs_t*)ctx, &blocks_used, &blocks_total);
    if (rc != BFS_OK) {
        return map_error(rc);
    }
    // VFS считает байты, BFS - блоки: пересчёт здесь, на границе, чтобы
    // оболочка не знала размер блока чужой ФС.
    if (used != NULL) {
        *used = blocks_used * BFS_BLOCK_SIZE;
    }
    if (total != NULL) {
        *total = blocks_total * BFS_BLOCK_SIZE;
    }
    return VFS_OK;
}

// Время даёт вызывающий: bfs_set_now - единственный вход, и адаптер здесь
// ничего не решает, а только передаёт.
static void bfs_set_now_op(void* ctx, uint32_t unix_secs)
{
    bfs_set_now((bfs_t*)ctx, unix_secs);
}

vfs_backend_t vfs_bfs_backend(bfs_t* fs)
{
    vfs_backend_t be;
    memset(&be, 0, sizeof(be));
    be.ctx = fs;
    be.stat = bfs_stat_op;
    be.create = bfs_create_op;
    be.mkdir = bfs_mkdir_op;
    be.unlink = bfs_unlink_op;
    be.rmdir = bfs_rmdir_op;
    be.rename = bfs_rename_op;
    be.readdir = bfs_readdir_op;
    be.read = bfs_read_op;
    be.write = bfs_write_op;
    be.truncate = bfs_truncate_op;
    be.usage = bfs_usage_op;
    be.set_now = bfs_set_now_op;
    return be;
}
