/*
 * exfat.c - exFAT ファイルシステム パーサ
 *
 * 旧 exfat_decode.c (Ver 1.1) のディレクトリ解析本体を、共通ライブラリ
 * (fatlib) と組み合わせてモジュール化したもの。
 *
 * タイムスタンプの UTC 変換:
 *   1. File Entry の UtcOffset フィールド (Win10 1903+) が非 0x00 → フィールド優先
 *   2. フィールド 0x00 かつ -tz 指定あり → フォールバック値で UTC 変換
 *   3. フィールド 0x00 かつ -tz 未指定   → ローカル時刻のまま出力
 * 列順・-tz の挙動・削除エントリ出力は旧ツールと完全に同一。
 *
 * exFAT ディレクトリエントリセット構造:
 *   [FILE Entry 0x85] + [Stream Ext 0xC0] + [File Name Ext(s) 0xC1 ...]
 *   削除時は InUse bit (bit7) がクリアされ 0x05 / 0x40 / 0x41 になる。
 */

#include "exfat.h"
#include "fatlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ============================================================
 *  定数
 * ============================================================ */

#define EXFAT_FS_NAME       "EXFAT   "

/* ディレクトリエントリタイプ (bit7=InUse) */
#define ET_FILE             0x85U
#define ET_FILE_DEL         0x05U
#define ET_STREAM           0xC0U
#define ET_STREAM_DEL       0x40U
#define ET_FNAME            0xC1U
#define ET_FNAME_DEL        0x41U
#define ET_EOD              0x00U

/* Stream Extension GeneralSecondaryFlags */
#define FLAG_NOFATCHAIN     0x02U

/* ファイル属性 */
#define FA_DIRECTORY        0x0010U

/* FAT エントリ特殊値 */
#define FAT_BAD             0xFFFFFFF7U
#define FAT_EOF_MIN         0xFFFFFFF8U

/* フルパス構築の親クラスタ sentinel */
#define CLUSTER_NONE        0U

/* ============================================================
 *  オンディスク構造体
 * ============================================================ */
#pragma pack(push, 1)

typedef struct {
    uint8_t  jump_boot[3];
    uint8_t  fs_name[8];               /* "EXFAT   " */
    uint8_t  must_be_zero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;               /* FAT 先頭 (セクタ単位) */
    uint32_t fat_length;
    uint32_t cluster_heap_offset;      /* クラスタヒープ先頭 (セクタ単位) */
    uint32_t cluster_count;
    uint32_t root_dir_first_cluster;
    uint32_t volume_serial_number;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t  bytes_per_sector_shift;   /* log2(セクタサイズ) */
    uint8_t  sectors_per_cluster_shift;
    uint8_t  number_of_fats;
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved[7];
    uint8_t  boot_code[390];
    uint16_t boot_signature;
} EXFAT_VBR;

typedef struct {
    uint8_t  entry_type;               /* 0x85 / 0x05 */
    uint8_t  secondary_count;
    uint16_t set_checksum;
    uint16_t file_attributes;
    uint16_t reserved1;
    uint32_t create_time;
    uint32_t last_modified_time;
    uint32_t last_accessed_time;
    uint8_t  create_10ms;
    uint8_t  last_modified_10ms;
    uint8_t  create_utc_offset;        /* 15分単位 UTC オフセット。0x00=未設定 */
    uint8_t  last_modified_utc_offset;
    uint8_t  last_accessed_utc_offset;
    uint8_t  reserved2[7];
} FILE_ENTRY;

typedef struct {
    uint8_t  entry_type;               /* 0xC0 / 0x40 */
    uint8_t  general_secondary_flags;
    uint8_t  reserved1;
    uint8_t  name_length;
    uint16_t name_hash;
    uint16_t reserved2;
    uint64_t valid_data_length;
    uint32_t reserved3;
    uint32_t first_cluster;
    uint64_t data_length;
} STREAM_EXT;

typedef struct {
    uint8_t  entry_type;               /* 0xC1 / 0x41 */
    uint8_t  general_secondary_flags;
    uint16_t file_name[15];            /* UTF-16LE */
} FNAME_EXT;

#pragma pack(pop)

/* ============================================================
 *  FAT 操作
 * ============================================================ */
static uint32_t fat_next(const fat_vol_t *vol, uint32_t cluster) {
    if (!vol->fat || cluster >= vol->fat_entries) return FAT_EOF_MIN;
    return vol->fat[cluster];
}

static int fat_is_eof(uint32_t val) { return val >= FAT_EOF_MIN; }

/* ============================================================
 *  ディレクトリデータ読み込み
 * ============================================================ */
static uint8_t *read_dir_data(const fat_vol_t *vol,
                              uint32_t first_cluster,
                              uint8_t  no_fat_chain,
                              uint64_t data_length,
                              uint64_t *out_bytes) {
    *out_bytes = 0;
    if (first_cluster < 2 || first_cluster > vol->cluster_count + 1)
        return NULL;

    uint64_t n_clus;
    if (no_fat_chain) {
        if (data_length == 0) data_length = vol->bytes_per_clus;
        n_clus = (data_length + (uint64_t)vol->bytes_per_clus - 1) /
                 vol->bytes_per_clus;
    } else {
        n_clus = 0;
        uint32_t c = first_cluster;
        while (c >= 2 && c <= vol->cluster_count + 1 &&
               !fat_is_eof(c) && c != FAT_BAD) {
            n_clus++;
            if (n_clus > 2000000ULL) break;
            c = fat_next(vol, c);
        }
    }

    if (n_clus == 0) return NULL;
    uint64_t total = n_clus * vol->bytes_per_clus;
    uint8_t *buf   = malloc(total);
    if (!buf) { perror("malloc read_dir_data"); return NULL; }

    uint64_t done = 0;
    if (no_fat_chain) {
        for (uint64_t i = 0; i < n_clus && done < total; i++) {
            ssize_t r = fat_full_pread(vol->fd, buf + done, vol->bytes_per_clus,
                                       fat_cluster_to_byte(vol, first_cluster + (uint32_t)i));
            if (r <= 0) break;
            done += (uint64_t)r;
        }
    } else {
        uint32_t c = first_cluster;
        while (c >= 2 && c <= vol->cluster_count + 1 &&
               !fat_is_eof(c) && c != FAT_BAD && done < total) {
            ssize_t r = fat_full_pread(vol->fd, buf + done, vol->bytes_per_clus,
                                       fat_cluster_to_byte(vol, c));
            if (r <= 0) break;
            done += (uint64_t)r;
            c = fat_next(vol, c);
        }
    }

    *out_bytes = done;
    return buf;
}

/* ============================================================
 *  タイムスタンプ整形
 *
 * ts            : 32bit exFAT タイムスタンプ
 * inc10ms       : 10ms インクリメント (0-199)
 * utc_off_field : File Entry の UtcOffset バイト値
 *                 0x00 = 未設定, 他 = (int8_t)値 × 15 分 が UTC オフセット
 * ============================================================ */
static void exfat_time_to_str(uint32_t ts, uint8_t inc10ms,
                              uint8_t utc_off_field, int tz_fallback_min,
                              char *buf, size_t buflen) {
    if (ts == 0) { buf[0] = '\0'; return; }

    /* exFAT タイムスタンプのビット展開 */
    int year   = ((ts >> 25) & 0x7F) + 1980;
    int month  = (ts >> 21) & 0x0F;
    int day    = (ts >> 16) & 0x1F;
    int hour   = (ts >> 11) & 0x1F;
    int minute = (ts >>  5) & 0x3F;
    int dsec   = (ts & 0x1F);
    int second = dsec * 2 + (int)(inc10ms / 100);
    int ms     = (int)(inc10ms % 100) * 10;

    if (month < 1 || month > 12 || day < 1 || day > 31) {
        buf[0] = '\0'; return;
    }

    /* UTC オフセット決定 */
    int eff_offset_min = TZ_NOT_SET;
    if (utc_off_field != 0x00) {
        /* UtcOffset フィールドが設定されている (Win10 1903+) */
        eff_offset_min = (int)(int8_t)utc_off_field * 15;
    } else if (tz_fallback_min != TZ_NOT_SET) {
        /* フィールド未設定: -tz フォールバックを使用 */
        eff_offset_min = tz_fallback_min;
    }

    fat_time_to_str(year, month, day, hour, minute, second, ms,
                    eff_offset_min, buf, buflen);
}

/* ============================================================
 *  ファイル名組み立て
 * ============================================================ */
static void build_filename(const uint8_t *dir_data, size_t dir_size,
                           size_t stream_off, int name_len,
                           char *name_out, size_t name_out_sz) {
    name_out[0] = '\0';
    if (name_len <= 0 || name_len > 255) return;
    uint8_t utf16_buf[510];
    int collected = 0, remain = name_len;
    size_t off = stream_off + 32;
    while (remain > 0 && off + 32 <= dir_size) {
        uint8_t et = dir_data[off];
        if (et != ET_FNAME && et != ET_FNAME_DEL) break;
        int n = (remain > 15) ? 15 : remain;
        memcpy(utf16_buf + collected * 2, dir_data + off + 2, (size_t)n * 2);
        collected += n; remain -= n; off += 32;
    }
    if (collected > 0)
        fat_utf16le_to_utf8(utf16_buf, (size_t)collected * 2,
                            name_out, name_out_sz);
}

/* ============================================================
 *  ディレクトリ走査 (Pass 1 / Pass 2 共用)
 * ============================================================ */
static void walk_dir(fat_vol_t *vol, uint32_t dir_cluster,
                     uint8_t no_fat_chain, uint64_t dir_data_len,
                     ScanPass pass, CsvCtx *ctx, int depth) {
    if (depth > MAX_WALK_DEPTH) {
        fprintf(stderr, "[警告] 再帰深度超過 cluster=%u\n", dir_cluster);
        return;
    }
    if (dir_cluster < 2 || dir_cluster > vol->cluster_count + 1) return;

    uint64_t dir_size = 0;
    uint8_t *dir_data = read_dir_data(vol, dir_cluster, no_fat_chain,
                                      dir_data_len, &dir_size);
    if (!dir_data || dir_size == 0) { free(dir_data); return; }

    size_t pos = 0;
    while (pos + 32 <= dir_size) {
        uint8_t etype = dir_data[pos];
        if (etype == ET_EOD) break;
        if (etype != ET_FILE && etype != ET_FILE_DEL) { pos += 32; continue; }

        const FILE_ENTRY *fe = (const FILE_ENTRY *)(dir_data + pos);
        int     in_use    = (etype == ET_FILE);
        uint8_t sec_count = fe->secondary_count;

        if (sec_count < 2) { pos += 32; continue; }
        if (pos + 32u * (1u + sec_count) > dir_size) { pos += 32; continue; }

        size_t  stream_off = pos + 32;
        uint8_t s_et       = dir_data[stream_off];
        if (s_et != ET_STREAM && s_et != ET_STREAM_DEL) { pos += 32; continue; }

        const STREAM_EXT *se = (const STREAM_EXT *)(dir_data + stream_off);
        uint8_t  name_len    = se->name_length;
        uint32_t first_clus  = se->first_cluster;
        uint64_t valid_len   = se->valid_data_length;
        uint64_t sub_dlen    = se->data_length;
        uint8_t  flags       = se->general_secondary_flags;
        uint8_t  sub_nfc     = (flags & FLAG_NOFATCHAIN) ? 1 : 0;
        int      is_dir      = (fe->file_attributes & FA_DIRECTORY) != 0;

        char name[MAX_NAME_UTF8] = "";
        build_filename(dir_data, dir_size, stream_off, name_len,
                       name, sizeof(name));
        if (name[0] == '\0') { pos += 32u * (1u + sec_count); continue; }

        /* ────────── Pass 1: ディレクトリテーブル構築 ────────── */
        if (pass == PASS1) {
            if (is_dir && in_use && first_clus >= 2) {
                int already = (fat_dir_find(vol, first_clus) != NULL);
                fat_dir_add(vol, first_clus, dir_cluster, name);
                if (!already)
                    walk_dir(vol, first_clus, sub_nfc, sub_dlen,
                             pass, ctx, depth + 1);
            }

        /* ────────── Pass 2: CSV 出力 ────────── */
        } else {
            char ts_mod[40], ts_acc[40], ts_cre[40];

            exfat_time_to_str(fe->last_modified_time,
                              fe->last_modified_10ms,
                              fe->last_modified_utc_offset,
                              vol->tz_fallback_min, ts_mod, sizeof(ts_mod));
            exfat_time_to_str(fe->last_accessed_time,
                              0,
                              fe->last_accessed_utc_offset,
                              vol->tz_fallback_min, ts_acc, sizeof(ts_acc));
            exfat_time_to_str(fe->create_time,
                              fe->create_10ms,
                              fe->create_utc_offset,
                              vol->tz_fallback_min, ts_cre, sizeof(ts_cre));

            const char *status = in_use ? "IN_USE"  : "DELETED";
            const char *type   = is_dir ? "DIR"     : "FILE";
            uint64_t    fsize  = is_dir ? 0 : valid_len;

            char fullpath[MAX_PATH_UTF8];
            fat_build_fullpath(vol, dir_cluster, name, fullpath, sizeof(fullpath));

            fprintf(ctx->fp, "%u,%u,%s,%s,",
                    first_clus, dir_cluster, status, type);
            fat_csv_write_field(ctx->fp, ts_mod);   fputc(',', ctx->fp);
            fat_csv_write_field(ctx->fp, ts_acc);   fputc(',', ctx->fp);
            fat_csv_write_field(ctx->fp, ts_cre);   fputc(',', ctx->fp);
            fat_csv_write_field(ctx->fp, name);     fputc(',', ctx->fp);
            fat_csv_write_field(ctx->fp, fullpath); fputc(',', ctx->fp);
            fprintf(ctx->fp, "%llu\n", (unsigned long long)fsize);

            if (in_use) ctx->in_use++; else ctx->deleted++;
            ctx->total++;

            if (is_dir && in_use && first_clus >= 2)
                walk_dir(vol, first_clus, sub_nfc, sub_dlen,
                         pass, ctx, depth + 1);
        }

        pos += 32u * (1u + sec_count);
    }

    free(dir_data);
}

/* ============================================================
 *  シグネチャ判定
 * ============================================================ */
int exfat_is_signature(const uint8_t *sector0) {
    const EXFAT_VBR *vbr = (const EXFAT_VBR *)sector0;
    return memcmp(vbr->fs_name, EXFAT_FS_NAME, 8) == 0;
}

/* ============================================================
 *  ボリュームデコード本体
 * ============================================================ */
int exfat_decode_volume(int fd, const char *dev,
                        const char *csv_path, int tz_fallback_min) {
    fat_vol_t vol;
    memset(&vol, 0, sizeof(vol));
    vol.fd              = fd;
    vol.tz_fallback_min = tz_fallback_min;

    /* ── VBR 読み込み ── */
    EXFAT_VBR vbr;
    if (fat_full_pread(fd, &vbr, sizeof(vbr), 0) != sizeof(vbr)) {
        fprintf(stderr, "Error: VBR 読み取り失敗\n");
        return 1;
    }

    /* ── ボリューム情報 ── */
    vol.bytes_per_sec  = 1u << vbr.bytes_per_sector_shift;
    uint32_t spc       = 1u << vbr.sectors_per_cluster_shift;
    vol.bytes_per_clus = vol.bytes_per_sec * spc;
    vol.fat_offset_b   = (uint64_t)vbr.fat_offset          * vol.bytes_per_sec;
    vol.heap_offset_b  = (uint64_t)vbr.cluster_heap_offset * vol.bytes_per_sec;
    vol.root_cluster   = vbr.root_dir_first_cluster;
    vol.cluster_count  = vbr.cluster_count;

    fprintf(stderr, "# デバイス              : %s\n", dev);
    fprintf(stderr, "# FS 種別               : exFAT %u.%02u\n",
            vbr.fs_revision >> 8, vbr.fs_revision & 0xFF);
    fprintf(stderr, "# シリアル番号          : %04X-%04X\n",
            vbr.volume_serial_number >> 16,
            vbr.volume_serial_number & 0xFFFF);
    fprintf(stderr, "# セクタサイズ          : %u B\n",   vol.bytes_per_sec);
    fprintf(stderr, "# クラスタサイズ        : %u B\n",   vol.bytes_per_clus);
    fprintf(stderr, "# クラスタ総数          : %u\n",     vol.cluster_count);
    fprintf(stderr, "# ルートクラスタ        : %u\n",     vol.root_cluster);
    fprintf(stderr, "# FAT 開始オフセット    : 0x%llx\n",
            (unsigned long long)vol.fat_offset_b);
    fprintf(stderr, "# クラスタヒープ開始    : 0x%llx\n",
            (unsigned long long)vol.heap_offset_b);

    /* タイムゾーンモード表示 */
    if (vol.tz_fallback_min != TZ_NOT_SET) {
        int sign  = (vol.tz_fallback_min >= 0) ? 1 : -1;
        int abs_m = vol.tz_fallback_min * sign;
        fprintf(stderr,
                "# タイムスタンプ        : UTC 変換モード\n"
                "#   UtcOffset フィールド: 設定済みエントリはフィールド優先\n"
                "#   フォールバック      : %c%02d:%02d "
                "(UtcOffset 未設定エントリに適用)\n"
                "#   列名               : *_utc\n",
                (sign >= 0) ? '+' : '-', abs_m / 60, abs_m % 60);
    } else {
        fprintf(stderr,
                "# タイムスタンプ        : UtcOffset フィールド設定済みのみ UTC 変換\n"
                "#   UtcOffset 未設定    : ローカル時刻のまま出力\n"
                "#   ヒント             : -tz +0900 でフォールバックを指定可能\n");
    }

    /* ── FAT をメモリに読み込む ── */
    vol.fat_entries = vol.cluster_count + 2;
    uint64_t fat_bytes = (uint64_t)vol.fat_entries * 4;
    vol.fat = malloc(fat_bytes);
    if (!vol.fat) { perror("malloc FAT"); return 1; }

    fprintf(stderr, "# FAT 読み込み中 (%llu MB)...\n",
            (unsigned long long)(fat_bytes >> 20));
    ssize_t fat_read = fat_full_pread(fd, vol.fat, fat_bytes,
                                      (off_t)vol.fat_offset_b);
    if (fat_read < (ssize_t)fat_bytes)
        fprintf(stderr, "[警告] FAT 読み取り一部不完全 (%lld / %llu bytes)\n",
                (long long)fat_read, (unsigned long long)fat_bytes);
    fprintf(stderr, "# FAT 読み込み完了\n");

    /* ── Pass 1: ディレクトリテーブル構築 ── */
    fprintf(stderr, "# Pass 1: ディレクトリテーブル構築中...\n");
    fat_dir_add(&vol, vol.root_cluster, CLUSTER_NONE, "");
    walk_dir(&vol, vol.root_cluster, 0, 0, PASS1, NULL, 0);
    fprintf(stderr, "# ディレクトリテーブル構築完了\n");

    /* ── CSV 出力先 ── */
    FILE *csv_fp = stdout;
    if (csv_path) {
        csv_fp = fopen(csv_path, "w");
        if (!csv_fp) {
            fprintf(stderr, "Error: '%s' を開けません: %s\n",
                    csv_path, strerror(errno));
            free(vol.fat);
            fat_dir_table_free(&vol);
            return 1;
        }
    }

    /* CSV ヘッダ */
    if (vol.tz_fallback_min != TZ_NOT_SET) {
        fprintf(csv_fp,
                "cluster_no,parent_cluster_no,status,type,"
                "modified_tm_utc,accessed_tm_utc,created_tm_utc,"
                "filename,fullpath,file_size\n");
    } else {
        fprintf(csv_fp,
                "cluster_no,parent_cluster_no,status,type,"
                "modified_tm,accessed_tm,created_tm,"
                "filename,fullpath,file_size\n");
    }

    /* ── Pass 2: CSV 出力 ── */
    fprintf(stderr, "# Pass 2: CSV 出力中...\n");
    CsvCtx ctx = { csv_fp, 0, 0, 0 };
    walk_dir(&vol, vol.root_cluster, 0, 0, PASS2, &ctx, 0);

    if (csv_fp != stdout) fclose(csv_fp);

    fprintf(stderr, "# ─────────────────────────────────\n");
    fprintf(stderr, "# 処理済みエントリ: %llu\n",  (unsigned long long)ctx.total);
    fprintf(stderr, "# IN_USE          : %llu\n",  (unsigned long long)ctx.in_use);
    fprintf(stderr, "# DELETED         : %llu\n",  (unsigned long long)ctx.deleted);
    if (csv_path)
        fprintf(stderr, "# 出力ファイル    : %s\n", csv_path);

    /* ── クリーンアップ ── */
    fat_dir_table_free(&vol);
    free(vol.fat);
    return 0;
}
