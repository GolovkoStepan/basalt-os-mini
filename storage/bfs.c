// Своя файловая система во flash. См. bfs.h - там устройство и причины.
//
// Порядок в файле: примитивы (CRC, битмап, блоки), inode, карта блоков файла,
// каталог, разбор пути, публичные операции.

#include "bfs.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Формат на диске
// ---------------------------------------------------------------------------

// "BSFS" в little-endian. Число, а не строка, чтобы сравнение было одним
// сравнением и не зависело от выравнивания.
#define BFS_SUPER_MAGIC 0x53465342u
#define BFS_SUPER_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t generation;
    uint32_t block_count;
    uint32_t root_inode;
    uint32_t inode_count;
    uint32_t inode_table[BFS_INODE_BLOCKS];
    uint32_t crc;  // CRC32 от начала структуры до этого поля
} bfs_super_disk_t;

typedef struct {
    uint32_t mode;  // 0 - узел свободен; иначе BFS_TYPE_FILE / BFS_TYPE_DIR
    uint32_t size;  // байт
    uint32_t mtime; // пока не заполняется: часов в системе нет
    uint32_t links;
    uint32_t direct[BFS_DIRECT_BLOCKS];
    uint32_t indirect;
    uint32_t double_indirect;
    uint32_t reserved[14];
} bfs_inode_disk_t;

typedef struct {
    uint32_t inode;  // 0 - запись свободна (надгробие после удаления)
    uint16_t type;
    uint8_t name_len;
    uint8_t reserved;
    char name[BFS_NAME_MAX];
} bfs_dirent_disk_t;

_Static_assert(sizeof(bfs_inode_disk_t) == BFS_INODE_SIZE, "inode должен быть 128 байт");
_Static_assert(sizeof(bfs_dirent_disk_t) == 32, "запись каталога должна быть 32 байта");
_Static_assert(sizeof(bfs_super_disk_t) <= BFS_SUPER_SLOT, "суперблок не влезает в слот");

#define BFS_DIRENTS_PER_BLOCK (BFS_BLOCK_SIZE / sizeof(bfs_dirent_disk_t))

// Смещение inode index внутри его блока таблицы.
#define BFS_INODE_OFFSET(index) (((index) % BFS_INODES_PER_BLOCK) * BFS_INODE_SIZE)
#define BFS_INODE_TABLE_INDEX(index) ((index) / BFS_INODES_PER_BLOCK)

// ---------------------------------------------------------------------------
// Рабочие буферы
//
// Три блока в статике, а не на стеке: стек 8 КБ (start.S), и один 4-КБ
// массив в нём съел бы половину. Разделены по назначению именно потому, что
// функции вложены: разбор каталога держит свой блок, пока карта блоков файла
// читает indirect в свой. Один общий буфер здесь - это порча данных через
// месяц, а не ошибка компиляции.
//
// ФС в системе одна, поэтому и буферы одни. Если появится вторая (например,
// раздел настроек), буферы переедут в bfs_t.
// ---------------------------------------------------------------------------

static _Alignas(8) uint8_t s_meta[BFS_BLOCK_SIZE];  // inode-таблица, indirect
static _Alignas(8) uint8_t s_dir[BFS_BLOCK_SIZE];   // блок каталога
static _Alignas(8) uint8_t s_data[BFS_BLOCK_SIZE];  // содержимое файла

// ---------------------------------------------------------------------------
// Примитивы
// ---------------------------------------------------------------------------

static uint32_t crc32(const void* data, size_t len)
{
    const uint8_t* p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            // Битовая реализация без таблицы: CRC считается на суперблоке
            // (60 байт) при каждом монтировании и каждом коммите, таблица на
            // 1 КБ ради этого не нужна.
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static bool bit_test(const uint8_t* map, uint32_t i)
{
    return (map[i >> 3] >> (i & 7u)) & 1u;
}

static void bit_set(uint8_t* map, uint32_t i)
{
    map[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

static void bit_clear(uint8_t* map, uint32_t i)
{
    map[i >> 3] &= (uint8_t)~(1u << (i & 7u));
}

static int dev_fail(bfs_t* fs)
{
    // После отказа устройства состояние в памяти всё равно разошлось с диском
    // (часть блоков уже записана). Продолжать нельзя: следующая ошибка будет
    // уже порчей. Единственный выход - перемонтировать.
    fs->broken = true;
    return BFS_ERR_IO;
}

static int fs_ready(bfs_t* fs)
{
    if (fs == NULL || !fs->mounted) {
        return BFS_ERR_ARG;
    }
    return fs->broken ? BFS_ERR_BROKEN : BFS_OK;
}

static int block_read(bfs_t* fs, uint32_t block, void* dst)
{
    if (block >= fs->block_count) {
        // Ссылка за пределы раздела - это порча метаданных, а не ошибка ввода.
        return BFS_ERR_FORMAT;
    }
    if (fs->dev.read(fs->dev.ctx, block * BFS_BLOCK_SIZE, dst, BFS_BLOCK_SIZE) != 0) {
        return dev_fail(fs);
    }
    return BFS_OK;
}

// Стереть блок и записать его целиком. Отдельная функция от block_read
// намеренно: NOR-flash нельзя писать поверх нестёртого, и все записи в ФС
// идут только через неё - тогда «забыл стереть» невозможно по построению.
static int block_program(bfs_t* fs, uint32_t block, const void* src)
{
    if (block >= fs->block_count) {
        return BFS_ERR_FORMAT;
    }
    if (fs->dev.erase(fs->dev.ctx, block * BFS_BLOCK_SIZE) != 0) {
        return dev_fail(fs);
    }
    if (fs->dev.write(fs->dev.ctx, block * BFS_BLOCK_SIZE, src, BFS_BLOCK_SIZE) != 0) {
        return dev_fail(fs);
    }
    return BFS_OK;
}

static int block_alloc(bfs_t* fs, uint32_t* out)
{
    // С нуля и вверх: так блоки одного файла ложатся рядом, и чтение подряд
    // идёт без прыжков. Wear leveling здесь не нужен - 14 МБ текста за жизнь
    // платы не дают и тысячи циклов на сектор.
    for (uint32_t i = 1; i < fs->block_count; ++i) {
        if (!bit_test(fs->bitmap, i) && !bit_test(fs->pending, i)) {
            bit_set(fs->bitmap, i);
            *out = i;
            return BFS_OK;
        }
    }
    return BFS_ERR_NO_SPACE;
}

// Освобождение откладывается до коммита: пока новый суперблок не записан, на
// диске живёт старый, который ссылается на этот блок. Отдать его под запись
// сейчас - значит стереть данные, на которые ещё указывает рабочая копия.
static void block_release(bfs_t* fs, uint32_t block)
{
    if (block != 0 && block < fs->block_count) {
        bit_set(fs->pending, block);
    }
}

// Записать блок по новому адресу и обновить указатель. Это и есть COW: старый
// блок освобождается, но только после коммита.
static int block_cow(bfs_t* fs, uint32_t* slot, const void* data)
{
    uint32_t fresh;
    int rc = block_alloc(fs, &fresh);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = block_program(fs, fresh, data);
    if (rc != BFS_OK) {
        return rc;
    }
    block_release(fs, *slot);
    *slot = fresh;
    fs->dirty = true;
    return BFS_OK;
}

// ---------------------------------------------------------------------------
// Суперблок
// ---------------------------------------------------------------------------

#define BFS_SUPER_RING (BFS_SUPER_SECTORS * BFS_SUPER_SLOTS)

static uint32_t super_offset(uint32_t ring)
{
    const uint32_t sector = ring / BFS_SUPER_SLOTS;
    const uint32_t slot = ring % BFS_SUPER_SLOTS;
    return sector * BFS_BLOCK_SIZE + slot * BFS_SUPER_SLOT;
}

static int super_write(bfs_t* fs)
{
    const uint32_t ring = fs->super_slot % BFS_SUPER_RING;
    const uint32_t slot = ring % BFS_SUPER_SLOTS;

    // Стираем сектор только при входе в него. К этому моменту живая копия
    // лежит в соседнем секторе: кольцо из двух секторов для того и нужно,
    // иначе стирание на границе оставило бы ФС без заголовка на 45 мс.
    if (slot == 0) {
        if (fs->dev.erase(fs->dev.ctx, (ring / BFS_SUPER_SLOTS) * BFS_BLOCK_SIZE) != 0) {
            return dev_fail(fs);
        }
    }

    bfs_super_disk_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = BFS_SUPER_MAGIC;
    sb.version = BFS_SUPER_VERSION;
    sb.generation = fs->generation;
    sb.block_count = fs->block_count;
    sb.root_inode = fs->root_inode;
    sb.inode_count = BFS_INODE_COUNT;
    memcpy(sb.inode_table, fs->inode_table, sizeof(sb.inode_table));
    sb.crc = crc32(&sb, sizeof(sb) - sizeof(sb.crc));

    if (fs->dev.write(fs->dev.ctx, super_offset(ring), &sb, sizeof(sb)) != 0) {
        return dev_fail(fs);
    }

    fs->super_slot = ring + 1;
    return BFS_OK;
}

// Коммит: записать суперблок (точка невозврата) и только потом вернуть
// освобождённые блоки в оборот.
static int fs_commit(bfs_t* fs)
{
    if (!fs->dirty) {
        return BFS_OK;
    }
    const uint32_t next = fs->generation + 1;
    fs->generation = next;
    const int rc = super_write(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    fs->dirty = false;
    for (uint32_t i = 0; i < fs->block_count; ++i) {
        if (bit_test(fs->pending, i)) {
            bit_clear(fs->bitmap, i);
            bit_clear(fs->pending, i);
        }
    }
    return BFS_OK;
}

// ---------------------------------------------------------------------------
// Inode
// ---------------------------------------------------------------------------

static int inode_load(bfs_t* fs, uint32_t index, bfs_inode_disk_t* out)
{
    if (index >= BFS_INODE_COUNT) {
        return BFS_ERR_ARG;
    }
    const int rc = block_read(fs, fs->inode_table[BFS_INODE_TABLE_INDEX(index)], s_meta);
    if (rc != BFS_OK) {
        return rc;
    }
    memcpy(out, s_meta + BFS_INODE_OFFSET(index), sizeof(*out));
    return BFS_OK;
}

static int inode_store(bfs_t* fs, uint32_t index, const bfs_inode_disk_t* in)
{
    if (index >= BFS_INODE_COUNT) {
        return BFS_ERR_ARG;
    }
    uint32_t* const slot = &fs->inode_table[BFS_INODE_TABLE_INDEX(index)];
    int rc = block_read(fs, *slot, s_meta);
    if (rc != BFS_OK) {
        return rc;
    }
    memcpy(s_meta + BFS_INODE_OFFSET(index), in, sizeof(*in));
    return block_cow(fs, slot, s_meta);
}

static int inode_alloc(bfs_t* fs, uint32_t* out)
{
    for (uint32_t tb = 0; tb < BFS_INODE_BLOCKS; ++tb) {
        const int rc = block_read(fs, fs->inode_table[tb], s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        for (uint32_t k = 0; k < BFS_INODES_PER_BLOCK; ++k) {
            const uint32_t index = tb * BFS_INODES_PER_BLOCK + k;
            if (index == 0) {
                // Ноль в поле inode записи каталога означает «свободно»,
                // поэтому нулевой inode не выдаётся никогда.
                continue;
            }
            uint32_t mode;
            memcpy(&mode, s_meta + k * BFS_INODE_SIZE, sizeof(mode));
            if (mode == 0) {
                *out = index;
                return BFS_OK;
            }
        }
    }
    return BFS_ERR_NO_SPACE;
}

// ---------------------------------------------------------------------------
// Карта блоков файла: прямые, indirect, double indirect
// ---------------------------------------------------------------------------

// Предельный размер файла. Считается один раз и используется как проверка
// индекса: за этой границей адресовать нечем.
static uint32_t bmap_capacity(void)
{
    return BFS_DIRECT_BLOCKS + BFS_PTRS_PER_BLOCK +
           BFS_PTRS_PER_BLOCK * BFS_PTRS_PER_BLOCK;
}

static int bmap_get(bfs_t* fs, const bfs_inode_disk_t* ino, uint32_t index, uint32_t* out)
{
    *out = 0;
    if (index >= bmap_capacity()) {
        return BFS_ERR_ARG;
    }
    if (index < BFS_DIRECT_BLOCKS) {
        *out = ino->direct[index];
        return BFS_OK;
    }
    index -= BFS_DIRECT_BLOCKS;

    if (index < BFS_PTRS_PER_BLOCK) {
        if (ino->indirect == 0) {
            return BFS_OK;
        }
        const int rc = block_read(fs, ino->indirect, s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        memcpy(out, s_meta + index * sizeof(uint32_t), sizeof(*out));
        return BFS_OK;
    }
    index -= BFS_PTRS_PER_BLOCK;

    if (ino->double_indirect == 0) {
        return BFS_OK;
    }
    const uint32_t l1 = index / BFS_PTRS_PER_BLOCK;
    const uint32_t l2 = index % BFS_PTRS_PER_BLOCK;
    int rc = block_read(fs, ino->double_indirect, s_meta);
    if (rc != BFS_OK) {
        return rc;
    }
    uint32_t mid;
    memcpy(&mid, s_meta + l1 * sizeof(uint32_t), sizeof(mid));
    if (mid == 0) {
        return BFS_OK;
    }
    rc = block_read(fs, mid, s_meta);
    if (rc != BFS_OK) {
        return rc;
    }
    memcpy(out, s_meta + l2 * sizeof(uint32_t), sizeof(*out));
    return BFS_OK;
}

// Создать промежуточный блок, заполненный нулями. Нули, а не 0xFF: стёртая
// flash - это 0xFF, и «пустой указатель» должен отличаться от «пустая flash»
// иначе обход после монтирования примет мусор за адреса.
static int zero_block_new(bfs_t* fs, uint32_t* out)
{
    uint32_t fresh;
    int rc = block_alloc(fs, &fresh);
    if (rc != BFS_OK) {
        return rc;
    }
    memset(s_meta, 0, BFS_BLOCK_SIZE);
    rc = block_program(fs, fresh, s_meta);
    if (rc != BFS_OK) {
        return rc;
    }
    *out = fresh;
    fs->dirty = true;
    return BFS_OK;
}

static int bmap_set(bfs_t* fs, bfs_inode_disk_t* ino, uint32_t index, uint32_t block)
{
    if (index >= bmap_capacity()) {
        return BFS_ERR_ARG;
    }
    if (index < BFS_DIRECT_BLOCKS) {
        ino->direct[index] = block;
        return BFS_OK;
    }
    index -= BFS_DIRECT_BLOCKS;

    if (index < BFS_PTRS_PER_BLOCK) {
        if (ino->indirect == 0) {
            uint32_t fresh;
            int rc = zero_block_new(fs, &fresh);
            if (rc != BFS_OK) {
                return rc;
            }
            memcpy(s_meta + index * sizeof(uint32_t), &block, sizeof(block));
            rc = block_program(fs, fresh, s_meta);
            if (rc != BFS_OK) {
                return rc;
            }
            ino->indirect = fresh;
            return BFS_OK;
        }
        int rc = block_read(fs, ino->indirect, s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        memcpy(s_meta + index * sizeof(uint32_t), &block, sizeof(block));
        return block_cow(fs, &ino->indirect, s_meta);
    }
    index -= BFS_PTRS_PER_BLOCK;

    const uint32_t l1 = index / BFS_PTRS_PER_BLOCK;
    const uint32_t l2 = index % BFS_PTRS_PER_BLOCK;

    uint32_t mid = 0;
    if (ino->double_indirect != 0) {
        int rc = block_read(fs, ino->double_indirect, s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        memcpy(&mid, s_meta + l1 * sizeof(uint32_t), sizeof(mid));
    }

    if (mid == 0) {
        uint32_t fresh;
        int rc = zero_block_new(fs, &fresh);
        if (rc != BFS_OK) {
            return rc;
        }
        memcpy(s_meta + l2 * sizeof(uint32_t), &block, sizeof(block));
        rc = block_program(fs, fresh, s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        mid = fresh;
    } else {
        int rc = block_read(fs, mid, s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        memcpy(s_meta + l2 * sizeof(uint32_t), &block, sizeof(block));
        uint32_t updated = mid;
        rc = block_cow(fs, &updated, s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        mid = updated;
    }

    // Указатель на mid лежит в блоке первого уровня: либо уже есть, либо его
    // надо создать. Записываем его последним - так mid уже готов.
    if (ino->double_indirect == 0) {
        uint32_t fresh;
        int rc = zero_block_new(fs, &fresh);
        if (rc != BFS_OK) {
            return rc;
        }
        memcpy(s_meta + l1 * sizeof(uint32_t), &mid, sizeof(mid));
        rc = block_program(fs, fresh, s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        ino->double_indirect = fresh;
        return BFS_OK;
    }

    int rc = block_read(fs, ino->double_indirect, s_meta);
    if (rc != BFS_OK) {
        return rc;
    }
    memcpy(s_meta + l1 * sizeof(uint32_t), &mid, sizeof(mid));
    return block_cow(fs, &ino->double_indirect, s_meta);
}

// ---------------------------------------------------------------------------
// Содержимое файла
// ---------------------------------------------------------------------------

static int file_read(bfs_t* fs, const bfs_inode_disk_t* ino, uint32_t offset, void* dst,
                     uint32_t len, uint32_t* done)
{
    if (done != NULL) {
        *done = 0;
    }
    if (offset >= ino->size) {
        return BFS_OK;
    }
    if (len > ino->size - offset) {
        len = ino->size - offset;
    }

    uint8_t* const out = (uint8_t*)dst;
    uint32_t total = 0;
    while (total < len) {
        const uint32_t pos = offset + total;
        const uint32_t index = pos / BFS_BLOCK_SIZE;
        const uint32_t within = pos % BFS_BLOCK_SIZE;
        uint32_t chunk = BFS_BLOCK_SIZE - within;
        if (chunk > len - total) {
            chunk = len - total;
        }

        uint32_t block;
        int rc = bmap_get(fs, ino, index, &block);
        if (rc != BFS_OK) {
            return rc;
        }
        if (block != 0) {
            rc = block_read(fs, block, s_data);
            if (rc != BFS_OK) {
                return rc;
            }
            memcpy(out + total, s_data + within, chunk);
        } else {
            // Дырка в файле читается нулями, а не 0xFF из стёртого сектора.
            memset(out + total, 0, chunk);
        }
        total += chunk;
    }
    if (done != NULL) {
        *done = total;
    }
    return BFS_OK;
}

static int file_write(bfs_t* fs, bfs_inode_disk_t* ino, uint32_t offset, const void* src,
                      uint32_t len, uint32_t* done)
{
    if (done != NULL) {
        *done = 0;
    }
    const uint8_t* const in = (const uint8_t*)src;
    uint32_t total = 0;
    while (total < len) {
        const uint32_t pos = offset + total;
        const uint32_t index = pos / BFS_BLOCK_SIZE;
        const uint32_t within = pos % BFS_BLOCK_SIZE;
        uint32_t chunk = BFS_BLOCK_SIZE - within;
        if (chunk > len - total) {
            chunk = len - total;
        }

        uint32_t old;
        int rc = bmap_get(fs, ino, index, &old);
        if (rc != BFS_OK) {
            return rc;
        }
        if (old != 0) {
            rc = block_read(fs, old, s_data);
            if (rc != BFS_OK) {
                return rc;
            }
        } else {
            memset(s_data, 0, BFS_BLOCK_SIZE);
        }
        memcpy(s_data + within, in + total, chunk);

        uint32_t fresh;
        rc = block_alloc(fs, &fresh);
        if (rc != BFS_OK) {
            return rc;
        }
        rc = block_program(fs, fresh, s_data);
        if (rc != BFS_OK) {
            return rc;
        }
        rc = bmap_set(fs, ino, index, fresh);
        if (rc != BFS_OK) {
            return rc;
        }
        // Старый блок данных освобождается только сейчас: bmap_set уже не
        // ссылается на него, но суперблок ещё не записан.
        block_release(fs, old);
        fs->dirty = true;
        total += chunk;
    }
    if (offset + total > ino->size) {
        ino->size = offset + total;
    }
    if (done != NULL) {
        *done = total;
    }
    return BFS_OK;
}

static int file_truncate(bfs_t* fs, bfs_inode_disk_t* ino, uint32_t size)
{
    if (size >= ino->size) {
        // Увеличение размер не меняет: дырка читается нулями. Файл станет
        // длиннее только при записи.
        if (size > ino->size) {
            ino->size = size;
            fs->dirty = true;
        }
        return BFS_OK;
    }

    const uint32_t old_blocks = (ino->size + BFS_BLOCK_SIZE - 1) / BFS_BLOCK_SIZE;
    const uint32_t keep = (size + BFS_BLOCK_SIZE - 1) / BFS_BLOCK_SIZE;

    // 1. Освободить блоки данных за новым размером.
    for (uint32_t i = keep; i < old_blocks; ++i) {
        uint32_t block;
        const int rc = bmap_get(fs, ino, i, &block);
        if (rc != BFS_OK) {
            return rc;
        }
        block_release(fs, block);
    }

    // 2. Обнулить указатели на них, иначе после монтирования они снова
    //    окажутся занятыми (обход идёт по inode, а не по битмапу).
    if (keep <= BFS_DIRECT_BLOCKS) {
        for (uint32_t i = keep; i < BFS_DIRECT_BLOCKS; ++i) {
            ino->direct[i] = 0;
        }
        if (ino->indirect != 0) {
            block_release(fs, ino->indirect);
            ino->indirect = 0;
        }
        if (ino->double_indirect != 0) {
            block_release(fs, ino->double_indirect);
            ino->double_indirect = 0;
        }
    } else if (keep <= BFS_DIRECT_BLOCKS + BFS_PTRS_PER_BLOCK) {
        if (ino->indirect != 0) {
            int rc = block_read(fs, ino->indirect, s_meta);
            if (rc != BFS_OK) {
                return rc;
            }
            const uint32_t from = keep - BFS_DIRECT_BLOCKS;
            for (uint32_t i = from; i < BFS_PTRS_PER_BLOCK; ++i) {
                uint32_t v;
                memcpy(&v, s_meta + i * sizeof(uint32_t), sizeof(v));
                block_release(fs, v);
                v = 0;
                memcpy(s_meta + i * sizeof(uint32_t), &v, sizeof(v));
            }
            rc = block_cow(fs, &ino->indirect, s_meta);
            if (rc != BFS_OK) {
                return rc;
            }
        }
        if (ino->double_indirect != 0) {
            // Хвост в double indirect ещё дальше: его держит только обход
            // первого уровня, освобождать нечего - блоков там не было.
            block_release(fs, ino->double_indirect);
            ino->double_indirect = 0;
        }
    } else {
        const uint32_t idx = keep - BFS_DIRECT_BLOCKS - BFS_PTRS_PER_BLOCK;
        const uint32_t l1_first = idx / BFS_PTRS_PER_BLOCK;
        const uint32_t l2_first = idx % BFS_PTRS_PER_BLOCK;

        int rc = block_read(fs, ino->double_indirect, s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        for (uint32_t l1 = l1_first; l1 < BFS_PTRS_PER_BLOCK; ++l1) {
            uint32_t mid;
            memcpy(&mid, s_meta + l1 * sizeof(uint32_t), sizeof(mid));
            if (mid == 0) {
                continue;
            }
            if (l1 != l1_first) {
                // Целиком лишний промежуточный блок: его содержимое уже
                // недостижимо, поэтому достаточно освободить его самого.
                block_release(fs, mid);
                mid = 0;
                memcpy(s_meta + l1 * sizeof(uint32_t), &mid, sizeof(mid));
                continue;
            }
            rc = block_read(fs, mid, s_dir);
            if (rc != BFS_OK) {
                return rc;
            }
            for (uint32_t l2 = l2_first; l2 < BFS_PTRS_PER_BLOCK; ++l2) {
                uint32_t v;
                memcpy(&v, s_dir + l2 * sizeof(uint32_t), sizeof(v));
                block_release(fs, v);
                v = 0;
                memcpy(s_dir + l2 * sizeof(uint32_t), &v, sizeof(v));
            }
            uint32_t updated = mid;
            rc = block_cow(fs, &updated, s_dir);
            if (rc != BFS_OK) {
                return rc;
            }
            memcpy(s_meta + l1 * sizeof(uint32_t), &updated, sizeof(updated));
        }
        rc = block_cow(fs, &ino->double_indirect, s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
    }

    ino->size = size;
    fs->dirty = true;
    return BFS_OK;
}

// ---------------------------------------------------------------------------
// Каталог
//
// Запись каталога - 32 байта, 128 записей в блоке, поэтому запись не
// пересекает границу блока. Удаление ставит inode = 0 (надгробие): каталог не
// сжимается, зато удаление - это одна запись, а не сдвиг всего содержимого.
// Пустые места переиспользуются при добавлении.
// ---------------------------------------------------------------------------

static bfs_dirent_disk_t* dir_entry_at(uint8_t* block, uint32_t k)
{
    return (bfs_dirent_disk_t*)(block + k * sizeof(bfs_dirent_disk_t));
}

static int dir_lookup(bfs_t* fs, const bfs_inode_disk_t* dir, const char* name,
                      bfs_dirent_disk_t* out, uint32_t* off_out)
{
    const size_t nlen = strlen(name);
    if (nlen == 0 || nlen > BFS_NAME_MAX) {
        return BFS_ERR_NAME;
    }
    const uint32_t nblocks = (dir->size + BFS_BLOCK_SIZE - 1) / BFS_BLOCK_SIZE;
    for (uint32_t bi = 0; bi < nblocks; ++bi) {
        uint32_t block;
        int rc = bmap_get(fs, dir, bi, &block);
        if (rc != BFS_OK) {
            return rc;
        }
        if (block == 0) {
            continue;
        }
        rc = block_read(fs, block, s_dir);
        if (rc != BFS_OK) {
            return rc;
        }
        for (uint32_t k = 0; k < BFS_DIRENTS_PER_BLOCK; ++k) {
            const bfs_dirent_disk_t* const e = dir_entry_at(s_dir, k);
            if (e->inode == 0 || e->name_len != nlen) {
                continue;
            }
            if (memcmp(e->name, name, nlen) == 0) {
                if (out != NULL) {
                    memcpy(out, e, sizeof(*out));
                }
                if (off_out != NULL) {
                    *off_out = bi * BFS_BLOCK_SIZE + k * sizeof(bfs_dirent_disk_t);
                }
                return BFS_OK;
            }
        }
    }
    return BFS_ERR_NOT_FOUND;
}

static int dir_add(bfs_t* fs, bfs_inode_disk_t* dir, const char* name, uint32_t inode,
                   uint16_t type)
{
    const size_t nlen = strlen(name);
    if (nlen == 0 || nlen > BFS_NAME_MAX) {
        return BFS_ERR_NAME;
    }

    bfs_dirent_disk_t e;
    memset(&e, 0, sizeof(e));
    e.inode = inode;
    e.type = type;
    e.name_len = (uint8_t)nlen;
    memcpy(e.name, name, nlen);

    // Сначала ищем надгробие - так каталог не растёт от каждой пересозданной
    // команды.
    const uint32_t nblocks = (dir->size + BFS_BLOCK_SIZE - 1) / BFS_BLOCK_SIZE;
    for (uint32_t bi = 0; bi < nblocks; ++bi) {
        uint32_t block;
        int rc = bmap_get(fs, dir, bi, &block);
        if (rc != BFS_OK) {
            return rc;
        }
        if (block == 0) {
            continue;
        }
        rc = block_read(fs, block, s_dir);
        if (rc != BFS_OK) {
            return rc;
        }
        for (uint32_t k = 0; k < BFS_DIRENTS_PER_BLOCK; ++k) {
            if (dir_entry_at(s_dir, k)->inode == 0) {
                return file_write(fs, dir, bi * BFS_BLOCK_SIZE + k * sizeof(e), &e, sizeof(e),
                                  NULL);
            }
        }
    }
    return file_write(fs, dir, dir->size, &e, sizeof(e), NULL);
}

static int dir_remove(bfs_t* fs, bfs_inode_disk_t* dir, const char* name)
{
    uint32_t off;
    int rc = dir_lookup(fs, dir, name, NULL, &off);
    if (rc != BFS_OK) {
        return rc;
    }
    const uint32_t zero = 0;
    return file_write(fs, dir, off, &zero, sizeof(zero), NULL);
}

// Сколько живых записей в каталоге - проверка «каталог пуст» перед rmdir.
static int dir_is_empty(bfs_t* fs, const bfs_inode_disk_t* dir, bool* empty)
{
    *empty = true;
    const uint32_t nblocks = (dir->size + BFS_BLOCK_SIZE - 1) / BFS_BLOCK_SIZE;
    for (uint32_t bi = 0; bi < nblocks; ++bi) {
        uint32_t block;
        int rc = bmap_get(fs, dir, bi, &block);
        if (rc != BFS_OK) {
            return rc;
        }
        if (block == 0) {
            continue;
        }
        rc = block_read(fs, block, s_dir);
        if (rc != BFS_OK) {
            return rc;
        }
        for (uint32_t k = 0; k < BFS_DIRENTS_PER_BLOCK; ++k) {
            if (dir_entry_at(s_dir, k)->inode != 0) {
                *empty = false;
                return BFS_OK;
            }
        }
    }
    return BFS_OK;
}

// ---------------------------------------------------------------------------
// Разбор пути
//
// Путь абсолютный; без ведущего '/' считается от корня - оболочка всё равно
// держит текущий каталог строкой и подставляет его сама. Компоненты "." и
// ".." не разбираются: их обрабатывает вызывающий, потому что текущий каталог
// известен только ему.
// ---------------------------------------------------------------------------

#define BFS_PATH_MAX 256u

// Найти inode по имени в каталоге.
static int dir_lookup_inode(bfs_t* fs, const bfs_inode_disk_t* dir, const char* name,
                            uint32_t* out)
{
    bfs_dirent_disk_t e;
    const int rc = dir_lookup(fs, dir, name, &e, NULL);
    if (rc != BFS_OK) {
        return rc;
    }
    *out = e.inode;
    return BFS_OK;
}

// Пройти путь целиком. Пустой path или "/" дают корень.
static int path_resolve(bfs_t* fs, const char* path, uint32_t* idx, bfs_inode_disk_t* out)
{
    if (path == NULL || path[0] == '\0') {
        return BFS_ERR_NAME;
    }
    if (strlen(path) >= BFS_PATH_MAX) {
        return BFS_ERR_NAME;
    }

    char buf[BFS_PATH_MAX];
    strcpy(buf, path);

    uint32_t cur = fs->root_inode;
    bfs_inode_disk_t cur_ino;
    int rc = inode_load(fs, cur, &cur_ino);
    if (rc != BFS_OK) {
        return rc;
    }

    char* p = buf;
    while (*p == '/') {
        ++p;
    }
    while (*p != '\0') {
        char* slash = strchr(p, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
        if (strcmp(p, ".") == 0 || p[0] == '\0') {
            // пропустить
        } else if (strcmp(p, "..") == 0) {
            // Переход вверх не поддержан: у inode нет ссылки на родителя,
            // а хранить её - это лишняя запись при каждом переименовании.
            return BFS_ERR_NAME;
        } else {
            // Компонент пути должен лежать в каталоге. Без этой проверки
            // "/файл/дальше" читал бы содержимое файла как записи каталога:
            // совпадения имени там случайны, но ошибка была бы неотличима от
            // «файла нет».
            if ((cur_ino.mode & BFS_TYPE_MASK) != BFS_TYPE_DIR) {
                return BFS_ERR_NOT_DIR;
            }
            uint32_t next;
            rc = dir_lookup_inode(fs, &cur_ino, p, &next);
            if (rc != BFS_OK) {
                return rc;
            }
            cur = next;
            rc = inode_load(fs, cur, &cur_ino);
            if (rc != BFS_OK) {
                return rc;
            }
        }
        if (slash == NULL) {
            break;
        }
        p = slash + 1;
        while (*p == '/') {
            ++p;
        }
    }

    // idx допускает NULL: stat и read индекс не нужны, им достаточно inode.
    if (idx != NULL) {
        *idx = cur;
    }
    if (out != NULL) {
        *out = cur_ino;
    }
    return BFS_OK;
}

// Разделить путь на родительский каталог и последний компонент.
static int path_parent(bfs_t* fs, const char* path, uint32_t* parent_idx,
                       bfs_inode_disk_t* parent, char* leaf)
{
    if (path == NULL || path[0] == '\0') {
        return BFS_ERR_NAME;
    }
    if (strlen(path) >= BFS_PATH_MAX) {
        return BFS_ERR_NAME;
    }

    char buf[BFS_PATH_MAX];
    strcpy(buf, path);

    // Хвостовые '/' отбрасываются: "/etc/" и "/etc" - один путь.
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }
    if (len == 0) {
        // Путь из одних слэшей - корень. Создавать и удалять корень нельзя.
        return BFS_ERR_NAME;
    }

    char* slash = strrchr(buf, '/');
    if (slash == NULL) {
        if (strlen(buf) > BFS_NAME_MAX) {
            return BFS_ERR_NAME;
        }
        strcpy(leaf, buf);
        *parent_idx = fs->root_inode;
        return inode_load(fs, fs->root_inode, parent);
    }

    *slash = '\0';
    const char* const dir_path = (buf[0] == '\0') ? "/" : buf;
    // Длину проверяем до копирования: leaf - 25 байт, а путь до 255, и strcpy
    // здесь был бы переполнением буфера на стеке.
    if (strlen(slash + 1) > BFS_NAME_MAX) {
        return BFS_ERR_NAME;
    }
    strcpy(leaf, slash + 1);
    if (leaf[0] == '\0') {
        return BFS_ERR_NAME;
    }
    return path_resolve(fs, dir_path, parent_idx, parent);
}

// ---------------------------------------------------------------------------
// Публичные операции
// ---------------------------------------------------------------------------

static int super_scan(bfs_t* fs)
{
    uint32_t best_generation = 0;
    uint32_t best_ring = 0;
    bool found = false;
    bfs_super_disk_t best;

    for (uint32_t sector = 0; sector < BFS_SUPER_SECTORS; ++sector) {
        // Читаем напрямую, а не через block_read: block_count ещё не известен -
        // он берётся из суперблока, который мы как раз ищем.
        if (fs->dev.read(fs->dev.ctx, sector * BFS_BLOCK_SIZE, s_meta, BFS_BLOCK_SIZE) != 0) {
            return dev_fail(fs);
        }
        for (uint32_t slot = 0; slot < BFS_SUPER_SLOTS; ++slot) {
            bfs_super_disk_t sb;
            memcpy(&sb, s_meta + slot * BFS_SUPER_SLOT, sizeof(sb));
            if (sb.magic != BFS_SUPER_MAGIC || sb.version != BFS_SUPER_VERSION) {
                continue;
            }
            if (sb.crc != crc32(&sb, sizeof(sb) - sizeof(sb.crc))) {
                continue;
            }
            if (!found || sb.generation > best_generation) {
                found = true;
                best_generation = sb.generation;
                best_ring = sector * BFS_SUPER_SLOTS + slot;
                best = sb;
            }
        }
    }

    if (!found) {
        return BFS_ERR_FORMAT;
    }
    if (best.block_count < 16 || best.block_count > BFS_MAX_BLOCKS) {
        return BFS_ERR_FORMAT;
    }
    if (best.root_inode == 0 || best.root_inode >= BFS_INODE_COUNT) {
        return BFS_ERR_FORMAT;
    }

    fs->block_count = best.block_count;
    fs->generation = best.generation;
    fs->root_inode = best.root_inode;
    fs->super_slot = (best_ring + 1) % BFS_SUPER_RING;
    memcpy(fs->inode_table, best.inode_table, sizeof(fs->inode_table));
    return BFS_OK;
}

static int mark_block(bfs_t* fs, uint32_t block)
{
    if (block >= fs->block_count) {
        return BFS_ERR_FORMAT;
    }
    bit_set(fs->bitmap, block);
    return BFS_OK;
}

static int bitmap_add_file(bfs_t* fs, const bfs_inode_disk_t* ino)
{
    for (uint32_t i = 0; i < BFS_DIRECT_BLOCKS; ++i) {
        if (ino->direct[i] != 0) {
            const int rc = mark_block(fs, ino->direct[i]);
            if (rc != BFS_OK) {
                return rc;
            }
        }
    }

    if (ino->indirect != 0) {
        int rc = mark_block(fs, ino->indirect);
        if (rc != BFS_OK) {
            return rc;
        }
        rc = block_read(fs, ino->indirect, s_dir);
        if (rc != BFS_OK) {
            return rc;
        }
        for (uint32_t i = 0; i < BFS_PTRS_PER_BLOCK; ++i) {
            uint32_t v;
            memcpy(&v, s_dir + i * sizeof(uint32_t), sizeof(v));
            if (v != 0) {
                rc = mark_block(fs, v);
                if (rc != BFS_OK) {
                    return rc;
                }
            }
        }
    }

    if (ino->double_indirect != 0) {
        int rc = mark_block(fs, ino->double_indirect);
        if (rc != BFS_OK) {
            return rc;
        }
        rc = block_read(fs, ino->double_indirect, s_data);
        if (rc != BFS_OK) {
            return rc;
        }
        for (uint32_t l1 = 0; l1 < BFS_PTRS_PER_BLOCK; ++l1) {
            uint32_t mid;
            memcpy(&mid, s_data + l1 * sizeof(uint32_t), sizeof(mid));
            if (mid == 0) {
                continue;
            }
            rc = mark_block(fs, mid);
            if (rc != BFS_OK) {
                return rc;
            }
            rc = block_read(fs, mid, s_dir);
            if (rc != BFS_OK) {
                return rc;
            }
            for (uint32_t l2 = 0; l2 < BFS_PTRS_PER_BLOCK; ++l2) {
                uint32_t v;
                memcpy(&v, s_dir + l2 * sizeof(uint32_t), sizeof(v));
                if (v != 0) {
                    rc = mark_block(fs, v);
                    if (rc != BFS_OK) {
                        return rc;
                    }
                }
            }
        }
    }
    return BFS_OK;
}

static int bitmap_rebuild(bfs_t* fs)
{
    memset(fs->bitmap, 0, sizeof(fs->bitmap));
    memset(fs->pending, 0, sizeof(fs->pending));

    int rc = mark_block(fs, 0);
    if (rc != BFS_OK) {
        return rc;
    }
    for (uint32_t i = 1; i < BFS_SUPER_SECTORS; ++i) {
        rc = mark_block(fs, i);
        if (rc != BFS_OK) {
            return rc;
        }
    }
    for (uint32_t i = 0; i < BFS_INODE_BLOCKS; ++i) {
        rc = mark_block(fs, fs->inode_table[i]);
        if (rc != BFS_OK) {
            return rc;
        }
    }

    // Обход по блокам таблицы: один блок читается один раз на 32 inode, а не
    // на каждый. При 256 inode это 8 чтений вместо 256.
    for (uint32_t tb = 0; tb < BFS_INODE_BLOCKS; ++tb) {
        rc = block_read(fs, fs->inode_table[tb], s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
        for (uint32_t k = 0; k < BFS_INODES_PER_BLOCK; ++k) {
            bfs_inode_disk_t ino;
            memcpy(&ino, s_meta + k * BFS_INODE_SIZE, sizeof(ino));
            if (ino.mode == 0) {
                continue;
            }
            // s_meta занят таблицей, поэтому содержимое файла обходится через
            // другие буферы.
            rc = bitmap_add_file(fs, &ino);
            if (rc != BFS_OK) {
                return rc;
            }
        }
    }
    return BFS_OK;
}

int bfs_format(bfs_t* fs, const bfs_device_t* dev, uint32_t block_count)
{
    if (fs == NULL || dev == NULL || dev->read == NULL || dev->erase == NULL ||
        dev->write == NULL) {
        return BFS_ERR_ARG;
    }
    // Меньше 16 блоков не хватит даже на служебные области: 2 суперблока,
    // 8 блоков inode-таблицы и хотя бы блок под корневой каталог.
    if (block_count < 16 || block_count > BFS_MAX_BLOCKS) {
        return BFS_ERR_ARG;
    }

    memset(fs, 0, sizeof(*fs));
    fs->dev = *dev;
    fs->block_count = block_count;

    // Оба сектора суперблока стираются сразу: иначе от прежней разметки
    // остались бы годные по CRC слоты с большим поколением, и монтирование
    // выбрало бы их.
    if (fs->dev.erase(fs->dev.ctx, 0) != 0 ||
        fs->dev.erase(fs->dev.ctx, BFS_BLOCK_SIZE) != 0) {
        return dev_fail(fs);
    }
    bit_set(fs->bitmap, 0);
    bit_set(fs->bitmap, 1);

    for (uint32_t i = 0; i < BFS_INODE_BLOCKS; ++i) {
        uint32_t block;
        int rc = block_alloc(fs, &block);
        if (rc != BFS_OK) {
            return rc;
        }
        fs->inode_table[i] = block;
    }

    uint32_t root_block;
    int rc = block_alloc(fs, &root_block);
    if (rc != BFS_OK) {
        return rc;
    }

    // Содержимое пишется один раз: таблица (с корневым inode в блоке 0) и
    // пустой корневой каталог.
    for (uint32_t i = 0; i < BFS_INODE_BLOCKS; ++i) {
        memset(s_meta, 0, BFS_BLOCK_SIZE);
        if (i == 0) {
            bfs_inode_disk_t root;
            memset(&root, 0, sizeof(root));
            root.mode = BFS_TYPE_DIR;
            root.links = 2;
            root.direct[0] = root_block;
            memcpy(s_meta + BFS_INODE_OFFSET(BFS_ROOT_INODE), &root, sizeof(root));
        }
        rc = block_program(fs, fs->inode_table[i], s_meta);
        if (rc != BFS_OK) {
            return rc;
        }
    }
    memset(s_meta, 0, BFS_BLOCK_SIZE);
    rc = block_program(fs, root_block, s_meta);
    if (rc != BFS_OK) {
        return rc;
    }

    fs->root_inode = BFS_ROOT_INODE;
    fs->generation = 0;
    fs->super_slot = 0;
    fs->mounted = true;
    fs->dirty = true;
    return fs_commit(fs);
}

int bfs_mount(bfs_t* fs, const bfs_device_t* dev)
{
    if (fs == NULL || dev == NULL || dev->read == NULL || dev->erase == NULL ||
        dev->write == NULL) {
        return BFS_ERR_ARG;
    }
    memset(fs, 0, sizeof(*fs));
    fs->dev = *dev;

    int rc = super_scan(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = bitmap_rebuild(fs);
    if (rc != BFS_OK) {
        memset(fs, 0, sizeof(*fs));
        return rc;
    }
    fs->mounted = true;
    fs->dirty = false;
    return BFS_OK;
}

int bfs_stat(bfs_t* fs, const char* path, uint32_t* size, uint16_t* type)
{
    int rc = fs_ready(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    bfs_inode_disk_t ino;
    rc = path_resolve(fs, path, NULL, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    if (size != NULL) {
        *size = ino.size;
    }
    if (type != NULL) {
        *type = (uint16_t)(ino.mode & BFS_TYPE_MASK);
    }
    return BFS_OK;
}

int bfs_create(bfs_t* fs, const char* path)
{
    int rc = fs_ready(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    uint32_t parent_idx;
    bfs_inode_disk_t parent;
    char leaf[BFS_NAME_MAX + 1];
    rc = path_parent(fs, path, &parent_idx, &parent, leaf);
    if (rc != BFS_OK) {
        return rc;
    }
    if ((parent.mode & BFS_TYPE_MASK) != BFS_TYPE_DIR) {
        return BFS_ERR_NOT_DIR;
    }
    rc = dir_lookup(fs, &parent, leaf, NULL, NULL);
    if (rc == BFS_OK) {
        return BFS_ERR_EXISTS;
    }
    if (rc != BFS_ERR_NOT_FOUND) {
        return rc;
    }

    uint32_t index;
    rc = inode_alloc(fs, &index);
    if (rc != BFS_OK) {
        return rc;
    }
    bfs_inode_disk_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.mode = BFS_TYPE_FILE;
    ino.links = 1;
    rc = inode_store(fs, index, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = dir_add(fs, &parent, leaf, index, (uint16_t)BFS_TYPE_FILE);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = inode_store(fs, parent_idx, &parent);
    if (rc != BFS_OK) {
        return rc;
    }
    return fs_commit(fs);
}

int bfs_mkdir(bfs_t* fs, const char* path)
{
    int rc = fs_ready(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    uint32_t parent_idx;
    bfs_inode_disk_t parent;
    char leaf[BFS_NAME_MAX + 1];
    rc = path_parent(fs, path, &parent_idx, &parent, leaf);
    if (rc != BFS_OK) {
        return rc;
    }
    if ((parent.mode & BFS_TYPE_MASK) != BFS_TYPE_DIR) {
        return BFS_ERR_NOT_DIR;
    }
    rc = dir_lookup(fs, &parent, leaf, NULL, NULL);
    if (rc == BFS_OK) {
        return BFS_ERR_EXISTS;
    }
    if (rc != BFS_ERR_NOT_FOUND) {
        return rc;
    }

    uint32_t index;
    rc = inode_alloc(fs, &index);
    if (rc != BFS_OK) {
        return rc;
    }
    uint32_t data_block;
    rc = block_alloc(fs, &data_block);
    if (rc != BFS_OK) {
        return rc;
    }
    memset(s_data, 0, BFS_BLOCK_SIZE);
    rc = block_program(fs, data_block, s_data);
    if (rc != BFS_OK) {
        return rc;
    }

    bfs_inode_disk_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.mode = BFS_TYPE_DIR;
    ino.links = 2;
    ino.direct[0] = data_block;
    rc = inode_store(fs, index, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = dir_add(fs, &parent, leaf, index, (uint16_t)BFS_TYPE_DIR);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = inode_store(fs, parent_idx, &parent);
    if (rc != BFS_OK) {
        return rc;
    }
    return fs_commit(fs);
}

// Общая часть unlink и rmdir: убрать запись из каталога и погасить inode.
static int remove_entry(bfs_t* fs, const char* path, uint16_t want_type)
{
    int rc = fs_ready(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    uint32_t parent_idx;
    bfs_inode_disk_t parent;
    char leaf[BFS_NAME_MAX + 1];
    rc = path_parent(fs, path, &parent_idx, &parent, leaf);
    if (rc != BFS_OK) {
        return rc;
    }

    uint32_t index;
    rc = dir_lookup_inode(fs, &parent, leaf, &index);
    if (rc != BFS_OK) {
        return rc;
    }
    bfs_inode_disk_t ino;
    rc = inode_load(fs, index, &ino);
    if (rc != BFS_OK) {
        return rc;
    }

    const uint16_t type = (uint16_t)(ino.mode & BFS_TYPE_MASK);
    if (type != want_type) {
        return (want_type == BFS_TYPE_FILE) ? BFS_ERR_IS_DIR : BFS_ERR_NOT_DIR;
    }
    if (type == BFS_TYPE_DIR) {
        bool empty = false;
        rc = dir_is_empty(fs, &ino, &empty);
        if (rc != BFS_OK) {
            return rc;
        }
        if (!empty) {
            return BFS_ERR_NOT_EMPTY;
        }
    }

    rc = dir_remove(fs, &parent, leaf);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = inode_store(fs, parent_idx, &parent);
    if (rc != BFS_OK) {
        return rc;
    }

    // Данные файла освобождаются целиком; пустой каталог держит один блок
    // под содержимое, его тоже надо вернуть.
    rc = file_truncate(fs, &ino, 0);
    if (rc != BFS_OK) {
        return rc;
    }
    ino.mode = 0;
    rc = inode_store(fs, index, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    return fs_commit(fs);
}

int bfs_unlink(bfs_t* fs, const char* path)
{
    return remove_entry(fs, path, (uint16_t)BFS_TYPE_FILE);
}

int bfs_rmdir(bfs_t* fs, const char* path)
{
    return remove_entry(fs, path, (uint16_t)BFS_TYPE_DIR);
}

int bfs_rename(bfs_t* fs, const char* from, const char* to)
{
    int rc = fs_ready(fs);
    if (rc != BFS_OK) {
        return rc;
    }

    uint32_t from_parent_idx;
    bfs_inode_disk_t from_parent;
    char from_leaf[BFS_NAME_MAX + 1];
    rc = path_parent(fs, from, &from_parent_idx, &from_parent, from_leaf);
    if (rc != BFS_OK) {
        return rc;
    }
    uint32_t index;
    rc = dir_lookup_inode(fs, &from_parent, from_leaf, &index);
    if (rc != BFS_OK) {
        return rc;
    }

    bfs_inode_disk_t ino;
    rc = inode_load(fs, index, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    const uint16_t type = (uint16_t)(ino.mode & BFS_TYPE_MASK);

    // Каталог нельзя переименовать «внутрь себя»: получилось бы дерево, в
    // котором узел является собственным потомком - обход по нему не
    // заканчивается никогда. Проверка по строкам, а не по inode: ссылки на
    // родителя у нас нет, а путь уже известен и он абсолютный.
    if (type == BFS_TYPE_DIR) {
        const size_t flen = strlen(from);
        if (strncmp(to, from, flen) == 0 && (to[flen] == '/' || to[flen] == '\0')) {
            return BFS_ERR_ARG;
        }
    }

    uint32_t to_parent_idx;
    bfs_inode_disk_t to_parent;
    char to_leaf[BFS_NAME_MAX + 1];
    rc = path_parent(fs, to, &to_parent_idx, &to_parent, to_leaf);
    if (rc != BFS_OK) {
        return rc;
    }
    if ((to_parent.mode & BFS_TYPE_MASK) != BFS_TYPE_DIR) {
        return BFS_ERR_NOT_DIR;
    }
    rc = dir_lookup(fs, &to_parent, to_leaf, NULL, NULL);
    if (rc == BFS_OK) {
        return BFS_ERR_EXISTS;
    }
    if (rc != BFS_ERR_NOT_FOUND) {
        return rc;
    }

    if (from_parent_idx == to_parent_idx) {
        // Один и тот же каталог: from_parent и to_parent - две копии одного
        // inode, и записать можно только одну. Работаем с to_parent, иначе
        // удаление, сделанное в from_parent, пропадёт при записи to_parent.
        rc = dir_remove(fs, &to_parent, from_leaf);
        if (rc != BFS_OK) {
            return rc;
        }
        rc = dir_add(fs, &to_parent, to_leaf, index, type);
        if (rc != BFS_OK) {
            return rc;
        }
        rc = inode_store(fs, to_parent_idx, &to_parent);
        if (rc != BFS_OK) {
            return rc;
        }
        return fs_commit(fs);
    }

    rc = dir_remove(fs, &from_parent, from_leaf);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = dir_add(fs, &to_parent, to_leaf, index, type);
    if (rc != BFS_OK) {
        return rc;
    }

    rc = inode_store(fs, from_parent_idx, &from_parent);
    if (rc == BFS_OK) {
        rc = inode_store(fs, to_parent_idx, &to_parent);
    }
    if (rc != BFS_OK) {
        return rc;
    }
    return fs_commit(fs);
}

int bfs_truncate(bfs_t* fs, const char* path, uint32_t size)
{
    int rc = fs_ready(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    uint32_t index;
    bfs_inode_disk_t ino;
    rc = path_resolve(fs, path, &index, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    if ((ino.mode & BFS_TYPE_MASK) != BFS_TYPE_FILE) {
        return BFS_ERR_IS_DIR;
    }
    rc = file_truncate(fs, &ino, size);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = inode_store(fs, index, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    return fs_commit(fs);
}

int bfs_read(bfs_t* fs, const char* path, uint32_t offset, void* dst, uint32_t len,
             uint32_t* done)
{
    int rc = fs_ready(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    bfs_inode_disk_t ino;
    rc = path_resolve(fs, path, NULL, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    if ((ino.mode & BFS_TYPE_MASK) == BFS_TYPE_DIR) {
        return BFS_ERR_IS_DIR;
    }
    return file_read(fs, &ino, offset, dst, len, done);
}

int bfs_write(bfs_t* fs, const char* path, uint32_t offset, const void* src, uint32_t len,
              uint32_t* done)
{
    int rc = fs_ready(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    uint32_t index;
    bfs_inode_disk_t ino;
    rc = path_resolve(fs, path, &index, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    if ((ino.mode & BFS_TYPE_MASK) == BFS_TYPE_DIR) {
        return BFS_ERR_IS_DIR;
    }
    rc = file_write(fs, &ino, offset, src, len, done);
    if (rc != BFS_OK) {
        return rc;
    }
    rc = inode_store(fs, index, &ino);
    if (rc != BFS_OK) {
        return rc;
    }
    return fs_commit(fs);
}

int bfs_readdir(bfs_t* fs, const char* path, uint32_t index, char* name, uint16_t* type,
                uint32_t* size)
{
    int rc = fs_ready(fs);
    if (rc != BFS_OK) {
        return rc;
    }
    bfs_inode_disk_t dir;
    rc = path_resolve(fs, path, NULL, &dir);
    if (rc != BFS_OK) {
        return rc;
    }
    if ((dir.mode & BFS_TYPE_MASK) != BFS_TYPE_DIR) {
        return BFS_ERR_NOT_DIR;
    }

    uint32_t seen = 0;
    const uint32_t nblocks = (dir.size + BFS_BLOCK_SIZE - 1) / BFS_BLOCK_SIZE;
    for (uint32_t bi = 0; bi < nblocks; ++bi) {
        uint32_t block;
        rc = bmap_get(fs, &dir, bi, &block);
        if (rc != BFS_OK) {
            return rc;
        }
        if (block == 0) {
            continue;
        }
        rc = block_read(fs, block, s_dir);
        if (rc != BFS_OK) {
            return rc;
        }
        for (uint32_t k = 0; k < BFS_DIRENTS_PER_BLOCK; ++k) {
            const bfs_dirent_disk_t* const e = dir_entry_at(s_dir, k);
            if (e->inode == 0) {
                continue;
            }
            if (seen++ != index) {
                continue;
            }
            if (name != NULL) {
                memcpy(name, e->name, e->name_len);
                name[e->name_len] = '\0';
            }
            if (type != NULL) {
                *type = e->type;
            }
            if (size != NULL) {
                bfs_inode_disk_t child;
                rc = inode_load(fs, e->inode, &child);
                if (rc != BFS_OK) {
                    return rc;
                }
                *size = child.size;
            }
            return BFS_OK;
        }
    }
    return BFS_ERR_NOT_FOUND;
}

int bfs_usage(const bfs_t* fs, uint32_t* used, uint32_t* total)
{
    if (fs == NULL || !fs->mounted) {
        return BFS_ERR_ARG;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < fs->block_count; ++i) {
        if (bit_test(fs->bitmap, i)) {
            ++count;
        }
    }
    if (used != NULL) {
        *used = count;
    }
    if (total != NULL) {
        *total = fs->block_count;
    }
    return BFS_OK;
}
