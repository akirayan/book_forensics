/*
 * regf_keys.c
 *
 * nk セル（キー）とサブキーリスト（lf/lh/ri/li）を辿るツリー走査層。
 * 値（vk）の解読は regf_value.c（process_vk_cell）に委ねる。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regf_main.h"
#include "regf_keys.h"
#include "regf_cell.h"
#include "regf_value.h"
#include "utf16le.h"
#include "timestamp.h"


/*
 * nk / vk のキー名を UTF-8 バッファへ。
 * is_ascii は nk->flags の ASCII ビット。raw_len はバイト長。
 * （utf16le_to_utf8 は code unit 数を取るので /2 する）
 */
static size_t decode_name(const char *raw, uint16_t raw_len, int is_ascii,
                          char *out, size_t out_len)
{
    if (is_ascii) {
        size_t n = (raw_len < out_len - 1) ? raw_len : out_len - 1;
        memcpy(out, raw, n);
        out[n] = '\0';
        return n;
    } else {
        return utf16le_to_utf8((const uint16_t *)raw, (uint16_t)(raw_len / 2),
                               out, out_len);
    }
}


/* ───────────────────────── nk（キー）───────────────────────── */
static void process_NK_cell(REGF_DECODE_INFO *rdi, uint32_t cell_offset,
                            char *path, int depth)
{
    if (depth > MAX_DEPTH) {
        fprintf(stderr, "# warning: max depth at 0x%08x\n", cell_offset);
        return;
    }

    uint8_t *p = regf_cell_ptr(rdi, cell_offset);
    if (!p) return;
    nk_cell_t *nk = (nk_cell_t *)p;

    /* ── キー名を path に追加（復帰は末尾で）── */
    size_t path_len = strlen(path);
    char name_utf8[2048];
    int is_ascii = (nk->flags & NK_FLAG_ASCII) ? 1 : 0;
    decode_name(nk->name, nk->name_length, is_ascii, name_utf8, sizeof(name_utf8));
    size_t name_len = strlen(name_utf8);

    if (path_len + 1 + name_len + 1 > MAX_PATH_LEN) {
        fprintf(stderr, "# warning: path too long\n");
        return;
    }
    path[path_len] = '/';
    memcpy(path + path_len + 1, name_utf8, name_len + 1);

    /*
     * -p PATH フィルタ:
     *   出力条件: path が filter_path 以下（対象キーとその子孫）
     *   再帰条件: 出力条件 OR filter_path が path 以下（対象の祖先）
     */
    int do_output = 1, do_recurse = 1;
    if (CHECK_OUTMODE(rdi->output_mode, OUT_FILTER_PATH) && rdi->filter_path) {
        const char *f = rdi->filter_path;
        size_t flen = strlen(f), plen = strlen(path);
        int path_under_filter = (strncmp(path, f, flen) == 0)
                                && (path[flen] == '/' || path[flen] == '\0');
        int filter_under_path = (strncmp(f, path, plen) == 0)
                                && (f[plen] == '/' || f[plen] == '\0' || plen == 0);
        int path_eq_filter    = (strcmp(path, f) == 0);
        do_output  = path_under_filter || path_eq_filter;
        do_recurse = do_output || filter_under_path;
        if (!do_recurse) { path[path_len] = '\0'; return; }
    }

    /* ── KEY 行: path|KEY|timestamp ── */
    if (do_output) {
        char ts[64];
        uint64_t ft = read_filetime((uint8_t *)&nk->last_written);
        filetime_to_str(ft, ts, sizeof(ts));
        printf("%s|KEY|%s\n", path, ts);
    }

    /* ── 値（vk）── */
    if (do_output && nk->value_count > 0 && nk->value_list_offset != 0xFFFFFFFF) {
        uint8_t *list = regf_cell_ptr(rdi, nk->value_list_offset);
        if (list) {
            uint32_t *voff = (uint32_t *)(list + 4);   /* セルサイズ(4B)の後が offset 配列 */
            for (uint32_t i = 0; i < nk->value_count; i++)
                process_vk_cell(rdi, voff[i], path);
        }
    }

    /* ── サブキー（lf/lh/ri/li を経由して nk へ）── */
    if (nk->subkey_count > 0 && nk->subkey_list_offset != 0xFFFFFFFF)
        enumerate_keys(rdi, nk->subkey_list_offset, path, depth + 1);

    path[path_len] = '\0';   /* path を復帰 */
}


/* ─────────── サブキーリスト lf / lh（entry = {nk_offset, hash}）─────────── */
static void process_LF_cell(REGF_DECODE_INFO *rdi, uint32_t cell_offset,
                            char *path, int depth)
{
    uint8_t *p = regf_cell_ptr(rdi, cell_offset);
    if (!p) return;
    lf_cell_t *lf = (lf_cell_t *)p;
    if (lf->cell_size > 0) return;   /* 正=解放済み */
    for (uint16_t i = 0; i < lf->count; i++)
        enumerate_keys(rdi, lf->entries[i].nk_offset, path, depth);
}

static void process_LH_cell(REGF_DECODE_INFO *rdi, uint32_t cell_offset,
                            char *path, int depth)
{
    uint8_t *p = regf_cell_ptr(rdi, cell_offset);
    if (!p) return;
    lh_cell_t *lh = (lh_cell_t *)p;
    if (lh->cell_size > 0) return;
    for (uint16_t i = 0; i < lh->count; i++)
        enumerate_keys(rdi, lh->entries[i].nk_offset, path, depth);
}

/* ─────────── ri（index of indexes。offset → lf/lh/li）─────────── */
static void process_RI_cell(REGF_DECODE_INFO *rdi, uint32_t cell_offset,
                            char *path, int depth)
{
    uint8_t *p = regf_cell_ptr(rdi, cell_offset);
    if (!p) return;
    ri_cell_t *ri = (ri_cell_t *)p;
    if (ri->cell_size > 0) return;
    for (uint16_t i = 0; i < ri->count; i++)
        enumerate_keys(rdi, ri->offsets[i], path, depth);   /* → lf/lh/li */
}

/* ─────────── li（list of nk。offset → nk。hash 無し）─────────── */
static void process_LI_cell(REGF_DECODE_INFO *rdi, uint32_t cell_offset,
                            char *path, int depth)
{
    uint8_t *p = regf_cell_ptr(rdi, cell_offset);
    if (!p) return;
    li_cell_t *li = (li_cell_t *)p;
    if (li->cell_size > 0) return;
    for (uint16_t i = 0; i < li->count; i++)
        enumerate_keys(rdi, li->offsets[i], path, depth);   /* → nk */
}


/*
 * セルの magic で振り分ける唯一の入口。
 * 扱うのはキー構造のセル（nk / lf / lh / ri / li）。値(vk)は process_NK_cell が
 * process_vk_cell を直接呼ぶので、ここには来ない。
 */
void enumerate_keys(REGF_DECODE_INFO *rdi, uint32_t cell_offset, char *path, int depth)
{
    uint8_t *cell_address = regf_cell_ptr(rdi, cell_offset);
    if (!cell_address) return;

    cell_header_t *cell_hdr = (cell_header_t *)cell_address;
    if (cell_hdr->cell_size >= 0) return;   /* 正 or 0 = 解放済み */

    if (memcmp(cell_hdr->magic, NK_MAGIC, 2) == 0) {
        process_NK_cell(rdi, cell_offset, path, depth);
    } else if (memcmp(cell_hdr->magic, LF_MAGIC, 2) == 0) {
        process_LF_cell(rdi, cell_offset, path, depth);
    } else if (memcmp(cell_hdr->magic, LH_MAGIC, 2) == 0) {
        process_LH_cell(rdi, cell_offset, path, depth);
    } else if (memcmp(cell_hdr->magic, RI_MAGIC, 2) == 0) {
        process_RI_cell(rdi, cell_offset, path, depth);
    } else if (memcmp(cell_hdr->magic, LI_MAGIC, 2) == 0) {
        process_LI_cell(rdi, cell_offset, path, depth);
    } else if (CHECK_OUTMODE(rdi->output_mode, OUT_VERBOSE)) {
        fprintf(stderr, "# warning: unexpected cell magic '%.2s' at 0x%08x\n",
                cell_hdr->magic, cell_offset);
    }
}
