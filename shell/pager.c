// Разметка страницы для `less`. См. pager.h - там про то, зачем отдельно.

#include "pager.h"

#include <string.h>

// Ширина табуляции. Восемь - как в матрице (`text_putc`), иначе один и тот же
// файл на экране и в `less` выглядел бы по-разному.
#define PAGER_TAB 8

void pager_init(pager* p, pager_io io)
{
    memset(p, 0, sizeof(*p));
    p->io = io;
    utf8_reset(&p->dec);
}

int pager_seek(pager* p, uint32_t offset)
{
    if (p->io.seek == NULL) {
        return -1;
    }
    const int rc = p->io.seek(p->io.ctx, offset);
    if (rc != 0) {
        return rc;
    }

    // Буфер выбрасываем целиком: в нём байты совсем другого места. База
    // ставится сразу, а не по первому чтению, иначе страница, собранная на
    // пустом буфере, сказала бы «конец потока».
    p->len = 0;
    p->pos = 0;
    p->base = offset;
    p->eof = false;
    p->error = 0;
    utf8_reset(&p->dec);
    return 0;
}

// Подтянуть очередной байт, не сдвигая позицию. false - поток кончился или
// отказал; различить это можно по p->error.
static bool pager_peek(pager* p, uint8_t* out)
{
    if (p->pos >= p->len) {
        if (p->eof) {
            return false;
        }

        uint32_t got = 0;
        const int rc = p->io.read(p->io.ctx, p->buf, PAGER_CHUNK, &got);
        if (rc != 0) {
            p->error = rc;
            p->eof = true;
            return false;
        }
        if (got == 0) {
            p->eof = true;
            return false;
        }

        // Адрес начала буфера - это конец предыдущего: сквозное смещение не
        // зависит от того, сколько раз пришлось дочитывать.
        p->base += p->len;
        p->len = got;
        p->pos = 0;
    }

    *out = p->buf[p->pos];
    return true;
}

static void pager_advance(pager* p)
{
    ++p->pos;
}

pager_page pager_fill(pager* p, uint16_t (*cells)[PAGER_COLS])
{
    for (int r = 0; r < PAGER_ROWS; ++r) {
        for (int c = 0; c < PAGER_COLS; ++c) {
            cells[r][c] = 0;
        }
    }

    pager_page page;
    page.rows = 0;
    page.eof = false;
    page.next = p->base + p->pos;

    // row - строка, в которую пойдёт следующий символ, а не последняя
    // заполненная: перевод строки закрывает текущую и сразу сдвигает эту
    // границу. Поэтому пустая строка внутри файла занимает строку экрана, а
    // перевод строки в самом конце - нет: после него граница уходит за
    // последнюю показанную строку, и лишнего места на экране не остаётся.
    int row = 0;
    int col = 0;

    while (row < PAGER_ROWS) {
        uint8_t b = 0;
        if (!pager_peek(p, &b)) {
            page.eof = true;
            break;
        }

        // Границу строки проверяем только на границе символа: разрезать UTF-8
        // пополам значило бы, что продолжение уйдёт на следующую страницу.
        if (p->dec.need == 0) {
            // Символ не влезает в остаток строки - начинаем новую. Проверка
            // здесь, а не после записи, потому что перенос нужен только тому
            // символу, который и правда не поместился.
            if (b != '\n' && b != '\r' && col >= PAGER_COLS) {
                ++row;
                col = 0;
                if (row >= PAGER_ROWS) {
                    break;
                }
            }

            // Табуляция проверяется целиком: разрезанная границей страницы,
            // она потеряла бы остаток пробелов, и текст после неё съехал бы
            // влево.
            if (b == '\t') {
                int r = row;
                int c = col + (PAGER_TAB - col % PAGER_TAB);
                while (c >= PAGER_COLS) {
                    c -= PAGER_COLS;
                    ++r;
                }
                if (r >= PAGER_ROWS) {
                    break;
                }
            }
        }

        pager_advance(p);

        if (b == '\n') {
            ++row;
            col = 0;
            continue;
        }

        // CR выбрасываем: файлы заливаются с мака, где конец строки - CRLF, и
        // видимая '^M' в конце каждой строки сделала бы чтение бессмысленным.
        if (b == '\r') {
            continue;
        }

        if (b == '\t') {
            const int need = PAGER_TAB - col % PAGER_TAB;
            for (int i = 0; i < need; ++i) {
                if (col >= PAGER_COLS) {
                    ++row;
                    col = 0;
                    if (row >= PAGER_ROWS) {
                        break;
                    }
                }
                cells[row][col++] = ' ';
            }
            continue;
        }

        uint32_t cp = 0;
        if (b < 0x80) {
            utf8_reset(&p->dec);
            cp = b;
        } else if (utf8_feed(&p->dec, b, &cp) != UTF8_DONE) {
            continue;
        }

        // Управляющий код глифа не имеет, а пропустить его - сдвинуть текст
        // влево. Вопрос - это знак, что в файле мусор, и его видно.
        if (cp < 0x20u || cp == 0x7fu) {
            cp = '?';
        }

        cells[row][col++] = (uint16_t)cp;
    }

    // Сколько строк показывать. Граница row - это первая строка, которую мы не
    // заполняли; если в неё уже что-то попало (строка без перевода на конце
    // файла), она тоже показывается.
    page.rows = (row < PAGER_ROWS) ? row + ((col > 0) ? 1 : 0) : PAGER_ROWS;

    // Смещение следующей страницы: первый непоказанный байт. Байт, который
    // прочитан в буфер, но не разобран, остаётся в буфере и не потеряется -
    // его номер и есть pos.
    page.next = p->base + p->pos;

    // Заглядываем на байт вперёд, чтобы конец файла был виден сразу на этой
    // странице. Иначе файл ровно на 16 строк показывал бы после себя пустую
    // страницу, и «конец» нажимался бы дважды.
    uint8_t ignored = 0;
    if (!pager_peek(p, &ignored)) {
        page.eof = true;
    }

    return page;
}
