/*
 * regf_value.c
 *
 * vk セル（レジストリ値）の解読と出力。evtx_decode の出力層に当たる層で、
 * 「値名・型・データ」を人が読める形に整える役目だけを持つ。
 * ツリー走査（nk/サブキー）とは分離してある。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regf_main.h"
#include "regf_value.h"
#include "regf_cell.h"
#include "utf16le.h"

#define MAX_BIN_DISPLAY        32   /* 通常出力でのバイナリ先頭表示バイト数 */
#define REGEDIT_BYTES_PER_LINE 25   /* -d の regedit 形式ダンプの1行バイト数 */

/* REG 型 → 文字列 */
static const char *reg_type_str(uint32_t t)
{
    switch (t) {
    case REG_NONE:      return "REG_NONE";
    case REG_SZ:        return "REG_SZ";
    case REG_EXPAND_SZ: return "REG_EXPAND_SZ";
    case REG_BINARY:    return "REG_BINARY";
    case REG_DWORD:     return "REG_DWORD";
    case REG_DWORD_BE:  return "REG_DWORD_BE";
    case REG_LINK:      return "REG_LINK";
    case REG_MULTI_SZ:  return "REG_MULTI_SZ";
    case REG_QWORD:     return "REG_QWORD";
    default:            return "REG_UNKNOWN";
    }
}

/* ワイルドカード (*, ?) マッチ。-d PATTERN 用。 */
static int wildcard_match(const char *pattern, const char *str)
{
    const char *p = pattern, *s = str;
    const char *star_p = NULL, *star_s = NULL;

    while (*s) {
        if (*p == '*') {
            star_p = ++p;
            star_s = s;
        } else if (*p == '?' || *p == *s) {
            p++; s++;
        } else if (star_p) {
            p = star_p;
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
}

/* REG_BINARY を "XX XX ..." で出力。full=0 なら先頭 MAX_BIN_DISPLAY で打ち切る。 */
static void print_binary(const uint8_t *data, uint32_t len, int full)
{
    uint32_t show = (full || len < MAX_BIN_DISPLAY) ? len : MAX_BIN_DISPLAY;
    for (uint32_t i = 0; i < show; i++) {
        if (i) putchar(' ');
        printf("%02X", data[i]);
    }
    if (!full && len > MAX_BIN_DISPLAY) printf(" ...[%u bytes]", len);
}

/* REG_MULTI_SZ を "str1 | str2 | ..." で出力（UTF-16LE、ダブル NUL 終端）。 */
static void print_multi_sz(const uint8_t *data, uint32_t len)
{
    uint32_t i = 0;
    int first = 1;
    char tmp[1024];

    while (i + 2 <= len) {
        if (data[i] == 0 && data[i + 1] == 0) break;   /* 終端 */

        uint32_t j = i;
        while (j + 2 <= len && !(data[j] == 0 && data[j + 1] == 0)) j += 2;

        utf16le_to_utf8((const uint16_t *)(data + i), (uint16_t)((j - i) / 2),
                        tmp, sizeof(tmp));
        if (!first) printf(" | ");
        printf("%s", tmp);
        first = 0;
        i = j + 2;
    }
}

/* -d 用：regedit 形式 ("name"=hex:XX,XX,\ ...) のフルダンプ。 */
static void print_binary_regedit(const char *vname, const uint8_t *data, uint32_t len)
{
    printf("\"%s\"=hex:", vname);
    for (uint32_t i = 0; i < len; i++) {
        if (i > 0 && i % REGEDIT_BYTES_PER_LINE == 0) printf("\n  ");
        printf("%02x", data[i]);
        if (i + 1 < len) {
            printf(",");
            if ((i + 1) % REGEDIT_BYTES_PER_LINE == 0) printf("\\");
        }
    }
    printf("\n");
}

/* 値名を UTF-8 に。is_ascii は vk->flags の ASCII ビット。raw_len はバイト長。 */
static void decode_value_name(const char *raw, uint16_t raw_len, int is_ascii,
                              char *out, size_t out_len)
{
    if (is_ascii) {
        size_t n = (raw_len < out_len - 1) ? raw_len : out_len - 1;
        memcpy(out, raw, n);
        out[n] = '\0';
    } else {
        utf16le_to_utf8((const uint16_t *)raw, (uint16_t)(raw_len / 2), out, out_len);
    }
}

void process_vk_cell(REGF_DECODE_INFO *rdi, uint32_t cell_offset, const char *key_path)
{
    uint8_t *p = regf_cell_ptr(rdi, cell_offset);
    if (!p) return;

    const vk_cell_t *vk = (const vk_cell_t *)p;
    if (memcmp(vk->magic, VK_MAGIC, 2) != 0) {
        if (CHECK_OUTMODE(rdi->output_mode, OUT_VERBOSE))
            fprintf(stderr, "# warning: expected 'vk', got '%.2s' at 0x%08X\n",
                    vk->magic, cell_offset);
        return;
    }

    int full = CHECK_OUTMODE(rdi->output_mode, OUT_FULL_BINARY);

    /* ── 値名 ── */
    char vname[1024];
    if (vk->name_length == 0) {
        snprintf(vname, sizeof(vname), "(default)");
    } else {
        int is_ascii = (vk->flags & VK_FLAG_ASCII) ? 1 : 0;
        decode_value_name(vk->name, vk->name_length, is_ascii, vname, sizeof(vname));
    }

    /* ── データの所在（インライン or 別セル）── */
    uint32_t raw_size  = vk->data_size;
    int      is_inline = (raw_size & VK_INLINE_FLAG) ? 1 : 0;
    uint32_t data_size = raw_size & ~VK_INLINE_FLAG;
    uint32_t data_type = vk->data_type;

    const uint8_t *data = NULL;
    uint8_t inline_buf[4];

    if (is_inline) {
        memcpy(inline_buf, &vk->data_offset, 4);   /* データが4バイトに直接 */
        data = inline_buf;
        if (data_size > 4) data_size = 4;
    } else if (vk->data_offset != 0xFFFFFFFF) {
        const uint8_t *cell = regf_cell_ptr(rdi, vk->data_offset);
        if (cell) data = cell + 4;                 /* セルヘッダ(4B)をスキップ */
    }

    /* ── 出力: key_path/値名|型| ── */
    printf("%s/%s|%s|", key_path, vname, reg_type_str(data_type));

    if (!data || data_size == 0) {
        printf("\n");
        return;
    }

    switch (data_type) {

    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_LINK: {
        /* REG_SZ を名乗るバイナリ（SAM の SID 等）を弾く簡易検証。 */
        int looks_binary = 0;
        if (data_size % 2 != 0) {
            looks_binary = 1;
        } else {
            uint32_t nwords = data_size / 2;
            uint32_t last = data[(nwords - 1) * 2] | (data[(nwords - 1) * 2 + 1] << 8);
            uint32_t check_until = (last == 0x0000) ? nwords - 1 : nwords;
            for (uint32_t wi = 0; wi < check_until; wi++) {
                uint16_t w = (uint16_t)data[wi * 2] | ((uint16_t)data[wi * 2 + 1] << 8);
                if (w == 0x0000 || w >= 0xFFFE) { looks_binary = 1; break; }
            }
        }
        if (looks_binary) {
            printf("[binary-as-SZ] ");
            print_binary(data, data_size, full);
        } else {
            char str[4096];
            utf16le_to_utf8((const uint16_t *)data, (uint16_t)(data_size / 2),
                            str, sizeof(str));
            printf("%s", str);
        }
        break;
    }

    case REG_DWORD:
        if (data_size >= 4)
            printf("0x%08X (%u)", *(const uint32_t *)data, *(const uint32_t *)data);
        break;

    case REG_DWORD_BE:
        if (data_size >= 4) {
            uint32_t v = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                       | ((uint32_t)data[2] <<  8) |  (uint32_t)data[3];
            printf("0x%08X (%u)", v, v);
        }
        break;

    case REG_QWORD:
        if (data_size >= 8) {
            uint64_t lo = *(const uint32_t *)data;
            uint64_t hi = *(const uint32_t *)(data + 4);
            uint64_t v  = (hi << 32) | lo;
            printf("0x%016llX (%llu)", (unsigned long long)v, (unsigned long long)v);
        }
        break;

    case REG_MULTI_SZ:
        print_multi_sz(data, data_size);
        break;

    case REG_BINARY:
    case REG_NONE:
    default:
        print_binary(data, data_size, full);
        break;
    }

    printf("\n");

    /* ── -d PATTERN: フルパスがマッチしたら regedit 形式でフルダンプ ── */
    if (CHECK_OUTMODE(rdi->output_mode, OUT_DUMP_PATTERN) &&
        rdi->dump_pattern && data_size > 0) {
        char fullpath[MAX_PATH_LEN + 1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", key_path, vname);
        if (wildcard_match(rdi->dump_pattern, fullpath))
            print_binary_regedit(vname, data, data_size);
    }
}
