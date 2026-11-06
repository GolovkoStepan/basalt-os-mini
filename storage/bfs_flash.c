// Привязка BFS к разделу встроенного flash. См. bfs_flash.h.

#include "bfs_flash.h"

#include <string.h>

#include "flash.h"
#include "parttable.h"

// Где таблица разделов и сколько в ней значащих байт. Числа из формата IDF:
// PlatformIO кладёт таблицу на 0x8000, и 0xC00 - это её полный размер без
// хвоста из 0xFF.
#define PARTITION_TABLE_ADDR 0x8000u
#define PARTITION_TABLE_MAX  0xC00u

// Буфер таблицы в статике, а не из кучи: во-первых, он нужен до того, как
// станет ясно, что куча работает; во-вторых, драйвер flash в режиме no-OS
// гасит кэш на время операции, поэтому буфер обязан быть в SRAM, а 3 КБ
// статики дешевле, чем проверка «куча дала внутреннюю память».
static uint8_t s_table[PARTITION_TABLE_MAX];

int basalt_fs_partition(const char* label, uint32_t* address, uint32_t* size)
{
    if (label == NULL) {
        return BASALT_FS_ERR_NO_PARTITION;
    }
    if (!basalt_flash_ready() && basalt_flash_init() != 0) {
        return BASALT_FS_ERR_NO_FLASH;
    }

    const int err = basalt_flash_read(PARTITION_TABLE_ADDR, s_table, sizeof(s_table));
    if (err != 0) {
        return err;
    }

    basalt_partition_t part;
    if (!basalt_parttable_find(s_table, sizeof(s_table), label, &part)) {
        return BASALT_FS_ERR_NO_PARTITION;
    }
    if (address != NULL) {
        *address = part.address;
    }
    if (size != NULL) {
        *size = part.size;
    }
    return 0;
}

// Блочное устройство поверх драйвера flash.
//
// Смещения BFS считает от начала раздела, а драйвер - от начала чипа, поэтому
// адаптер держит базу и границы. Границы проверяются здесь намеренно: ошибка в
// карте блоков не должна превращаться в стирание чужого раздела - соседний
// раздел это загрузочный образ, и «ФС испортилась» стало бы «плата не грузится».
typedef struct {
    uint32_t base;
    uint32_t size;
} partition_dev_t;

static int dev_read(void* ctx, uint32_t offset, void* dst, uint32_t len)
{
    const partition_dev_t* const p = (const partition_dev_t*)ctx;
    if (offset > p->size || len > p->size - offset) {
        return -1;
    }
    return basalt_flash_read(p->base + offset, dst, len);
}

static int dev_erase(void* ctx, uint32_t offset)
{
    const partition_dev_t* const p = (const partition_dev_t*)ctx;
    if (offset > p->size || BFS_BLOCK_SIZE > p->size - offset ||
        offset % BASALT_FLASH_SECTOR_SIZE != 0u) {
        return -1;
    }
    return basalt_flash_erase_sector(p->base + offset);
}

static int dev_write(void* ctx, uint32_t offset, const void* src, uint32_t len)
{
    const partition_dev_t* const p = (const partition_dev_t*)ctx;
    if (offset > p->size || len > p->size - offset) {
        return -1;
    }
    return basalt_flash_write(p->base + offset, src, len);
}

// Раздел, проверенный на пригодность, и устройство поверх него. Оба в статике
// по той же причине, что и таблица: один раздел - одно устройство, а держать
// его нужно всё время жизни ФС.
static partition_dev_t s_dev;

static int prepare(const char* label, bfs_device_t* out)
{
    uint32_t address = 0;
    uint32_t size = 0;
    const int rc = basalt_fs_partition(label, &address, &size);
    if (rc != 0) {
        return rc;
    }

    if (address % BASALT_FLASH_SECTOR_SIZE != 0u) {
        return BASALT_FS_ERR_BAD_PARTITION;
    }
    const uint32_t blocks = size / BFS_BLOCK_SIZE;
    if (blocks < 16u || blocks > BFS_MAX_BLOCKS) {
        return BASALT_FS_ERR_BAD_PARTITION;
    }

    s_dev.base = address;
    s_dev.size = size;

    out->ctx = &s_dev;
    out->read = dev_read;
    out->erase = dev_erase;
    out->write = dev_write;
    return 0;
}

int basalt_fs_mount(bfs_t* fs, const char* label, bool format_if_empty, bool* formatted)
{
    if (fs == NULL) {
        return BFS_ERR_ARG;
    }
    if (formatted != NULL) {
        *formatted = false;
    }

    bfs_device_t dev;
    const int rc = prepare(label, &dev);
    if (rc != 0) {
        return rc;
    }

    const int mount_rc = bfs_mount(fs, &dev);
    if (mount_rc == BFS_OK) {
        return 0;
    }
    if (mount_rc != BFS_ERR_FORMAT || !format_if_empty) {
        return mount_rc;
    }

    // Раздел не размечен - значит, на нём либо пусто, либо чужая разметка.
    // Второе опаснее: стирание суперблока от чужой ФС необратимо. Поэтому
    // решение «размечать» принимает вызывающий (format_if_empty), а не этот
    // код: у щупа это норма, у системы - только при первом запуске.
    const int fmt_rc = bfs_format(fs, &dev, s_dev.size / BFS_BLOCK_SIZE);
    if (fmt_rc != BFS_OK) {
        return fmt_rc;
    }
    if (formatted != NULL) {
        *formatted = true;
    }

    // Монтирование после разметки - не формальность: format оставил состояние
    // в памяти, а работать надо с тем, что лежит на диске.
    return bfs_mount(fs, &dev);
}

int basalt_fs_format(bfs_t* fs, const char* label)
{
    if (fs == NULL) {
        return BFS_ERR_ARG;
    }
    bfs_device_t dev;
    const int rc = prepare(label, &dev);
    if (rc != 0) {
        return rc;
    }
    return bfs_format(fs, &dev, s_dev.size / BFS_BLOCK_SIZE);
}
