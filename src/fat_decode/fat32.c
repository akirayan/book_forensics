/*
 * fat32.c - FAT32/VFAT ファイルシステム パーサ
 *
 * 旧 fat32_decode.c (Ver 1.0) のディレクトリ解析本体を、共通ライブラリ
 * (fatlib) と組み合わせてモジュール化したもの。
 *
 * FAT チェーンをたどり、8.3 短名と LFN (長ファイル名) を組み立て、
 * サブディレクトリへ再帰し、削除エントリ (先頭 0xE5) も拾って CSV 出力する。
 * 列順・-tz の挙動・削除エントリ出力は旧ツールと完全に同一。
 *
 * FAT32 ディレクトリ構造:
 *   各エントリ 32 バイト固定長。
 *   長ファイル名 (LFN): 属性 0x0F のエントリを短名エントリ直前に逆順で配置。
 *   削除エントリ: 短名エントリの name[0] が 0xE5 になる。LFN エントリは 0xE5。
 *   ディレクトリ終端: name[0] == 0x00
 */

#include "fat32.h"
#include "fatlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ============================================================
 *  定数
 * ============================================================ */

/* BPB 識別文字列 */
#define FAT32_FS_TYPE   "FAT32   "

/* ディレクトリエントリ属性 */
#define ATTR_VOLUME_ID  0x08U
#define ATTR_DIRECTORY  0x10U
#define ATTR_LFN        0x0FU   /* LFN エントリのマーカー (RO|HID|SYS|VOL) */

/* 削除マーク */
#define DIRENT_DELETED  0xE5U   /* name[0] のこの値 = 削除済み */
#define DIRENT_END      0x00U   /* name[0] のこの値 = ディレクトリ終端 */
/* 0xE5 が本来の先頭文字 (日本語環境の "。" など) だった場合の代替 */
#define DIRENT_KANJI    0x05U   /* 実際の先頭バイトは 0xE5 として扱う */

/* FAT32 クラスタ特殊値 (下位 28bit で判定) */
#define FAT32_MASK      0x0FFFFFFFU
#define FAT32_FREE      0x00000000U
#define FAT32_BAD       0x0FFFFFF7U
#define FAT32_EOF_MIN   0x0FFFFFF8U

/* LFN 1 エントリあたりの文字数 */
#define LFN_CHARS_PER_ENTRY  13
#define LFN_MAX_ENTRIES      20      /* 255 / 13 = 19.6 → 20 エントリ上限 */
#define LFN_MAX_CHARS        (LFN_MAX_ENTRIES * LFN_CHARS_PER_ENTRY + 1)

/* ============================================================
 *  オンディスク構造体
 * ============================================================ */
#pragma pack(push, 1)

/*
 * FAT32 BPB (BIOS Parameter Block)
 * パーティション先頭 512 バイトに配置。
 */
typedef struct {
    uint8_t  jump_boot[3];          /* 0x00: EB xx 90 */
    uint8_t  oem_name[8];           /* 0x03: "MSDOS5.0" など */
    uint16_t bytes_per_sector;      /* 0x0B: セクタサイズ (通常 512) */
    uint8_t  sectors_per_cluster;   /* 0x0D: クラスタあたりセクタ数 */
    uint16_t reserved_sectors;      /* 0x0E: 予約セクタ数 (BPB を含む) */
    uint8_t  num_fats;              /* 0x10: FAT コピー数 (通常 2) */
    uint16_t root_entry_count;      /* 0x11: FAT32 では 0 */
    uint16_t total_sectors_16;      /* 0x13: FAT32 では 0 */
    uint8_t  media_type;            /* 0x15: 0xF8=固定ディスク */
    uint16_t fat_size_16;           /* 0x16: FAT32 では 0 */
    uint16_t sectors_per_track;     /* 0x18 */
    uint16_t num_heads;             /* 0x1A */
    uint32_t hidden_sectors;        /* 0x1C: パーティション前のセクタ数 */
    uint32_t total_sectors_32;      /* 0x20: 総セクタ数 */
    /* --- FAT32 拡張 BPB (オフセット 0x24 以降) --- */
    uint32_t fat_size_32;           /* 0x24: FAT のセクタ数 */
    uint16_t ext_flags;             /* 0x28: ミラーリングフラグ */
    uint16_t fs_version;            /* 0x2A: 0x0000 */
    uint32_t root_cluster;          /* 0x2C: ルートディレクトリ先頭クラスタ */
    uint16_t fs_info;               /* 0x30: FSInfo セクタ番号 */
    uint16_t backup_boot;           /* 0x32: バックアップブートセクタ */
    uint8_t  reserved[12];          /* 0x34 */
    uint8_t  drive_number;          /* 0x40 */
    uint8_t  reserved1;             /* 0x41 */
    uint8_t  boot_sig;              /* 0x42: 0x29 = 拡張シグネチャ */
    uint32_t volume_id;             /* 0x43: ボリュームシリアル番号 */
    uint8_t  volume_label[11];      /* 0x47: ボリュームラベル */
    uint8_t  fs_type[8];            /* 0x52: "FAT32   " */
} FAT32_BPB;

/*
 * FAT32 ディレクトリエントリ (32 バイト)
 * 属性 (attr) が ATTR_LFN (0x0F) のときは LFN エントリとして扱う。
 */
typedef struct {
    uint8_t  name[8];           /* 0x00: 短名 (8 文字, 空白埋め) */
    uint8_t  ext[3];            /* 0x08: 拡張子 (3 文字, 空白埋め) */
    uint8_t  attr;              /* 0x0B: 属性 */
    uint8_t  nt_res;            /* 0x0C: Windows NT 予約 (大小文字フラグ) */
    uint8_t  create_time_tenth; /* 0x0D: 作成時刻の 10ms 単位補正 (0-199) */
    uint16_t create_time;       /* 0x0E: 作成時刻 */
    uint16_t create_date;       /* 0x10: 作成日付 */
    uint16_t access_date;       /* 0x12: 最終アクセス日付 (時刻なし) */
    uint16_t cluster_high;      /* 0x14: 先頭クラスタ上位 16bit */
    uint16_t write_time;        /* 0x16: 最終更新時刻 */
    uint16_t write_date;        /* 0x18: 最終更新日付 */
    uint16_t cluster_low;       /* 0x1A: 先頭クラスタ下位 16bit */
    uint32_t file_size;         /* 0x1C: ファイルサイズ (DIR は 0) */
} FAT32_DIRENT;

/*
 * LFN エントリ (属性 0x0F, 32 バイト)
 * 短名エントリの直前に order の降順で並ぶ。
 * order & 0x3F が 1-based のエントリ番号、0x40 bit = 最終エントリ。
 */
typedef struct {
    uint8_t  order;         /* 0x00: エントリ番号 (1始まり, 0x40=最終) */
    uint16_t name1[5];      /* 0x01: UTF-16LE 文字 (5 文字) */
    uint8_t  attr;          /* 0x0B: 0x0F */
    uint8_t  type;          /* 0x0C: 0x00 */
    uint8_t  checksum;      /* 0x0D: 短名のチェックサム */
    uint16_t name2[6];      /* 0x0E: UTF-16LE 文字 (6 文字) */
    uint16_t cluster;       /* 0x1A: 0x0000 */
    uint16_t name3[2];      /* 0x1C: UTF-16LE 文字 (2 文字) */
} FAT32_LFN;

#pragma pack(pop)

/* ============================================================
 *  FAT 操作
 * ============================================================ */

/* FAT チェーンで次のクラスタ番号を得る (28bit マスク済み) */
static uint32_t fat_next(const fat_vol_t *vol, uint32_t cluster) {
    if (!vol->fat || cluster >= vol->fat_entries) return FAT32_EOF_MIN;
    return vol->fat[cluster] & FAT32_MASK;
}

static int fat_is_eof(uint32_t val) {
    return (val & FAT32_MASK) >= FAT32_EOF_MIN;
}

static int fat_is_free(uint32_t val) {
    return (val & FAT32_MASK) == FAT32_FREE;
}

/* ============================================================
 *  クラスタチェーン読み込み
 *
 *  FAT チェーンを辿り、ディレクトリデータ全体をメモリに読む。
 *  返り値: 確保したバッファ (呼び出し元が free)。*out_bytes = 読んだバイト数。
 * ============================================================ */
static uint8_t *read_cluster_chain(const fat_vol_t *vol,
                                   uint32_t first_cluster,
                                   uint64_t *out_bytes) {
    *out_bytes = 0;
    if (first_cluster < 2 || fat_is_free(first_cluster) ||
        fat_is_eof(first_cluster)) return NULL;

    /* クラスタ数をカウント */
    uint64_t n_clus = 0;
    uint32_t c = first_cluster;
    while (c >= 2 && c <= vol->cluster_count + 1 &&
           !fat_is_eof(c) && (c & FAT32_MASK) != FAT32_BAD) {
        n_clus++;
        if (n_clus > 2000000ULL) break;
        c = fat_next(vol, c);
    }
    if (n_clus == 0) return NULL;

    uint64_t total = n_clus * vol->bytes_per_clus;
    uint8_t *buf   = malloc(total);
    if (!buf) { perror("malloc read_cluster_chain"); return NULL; }

    uint64_t done = 0;
    c = first_cluster;
    while (c >= 2 && c <= vol->cluster_count + 1 &&
           !fat_is_eof(c) && (c & FAT32_MASK) != FAT32_BAD && done < total) {
        ssize_t r = fat_full_pread(vol->fd, buf + done, vol->bytes_per_clus,
                                   fat_cluster_to_byte(vol, c));
        if (r <= 0) break;
        done += (uint64_t)r;
        c = fat_next(vol, c);
    }

    *out_bytes = done;
    return buf;
}

/* ============================================================
 *  タイムスタンプ整形
 *
 * FAT32 date (16bit):
 *   bits 15:9  Year - 1980 / bits 8:5 Month (1-12) / bits 4:0 Day (1-31)
 * FAT32 time (16bit):
 *   bits 15:11 Hour / bits 10:5 Minute / bits 4:0 DoubleSecond (0-29 → ×2)
 * tenth: 10ms インクリメント (0-199)、作成時刻のみ存在。
 *
 * tz_fallback_min 指定あり → UTC 変換、なし → ローカル時刻のまま出力。
 * date == 0 → 空文字列 (アクセス日付未設定のケース)。
 * ============================================================ */
static void fat32_datetime_to_str(uint16_t date, uint16_t time_val,
                                  uint8_t tenth, int tz_fallback_min,
                                  char *buf, size_t buflen) {
    if (date == 0) { buf[0] = '\0'; return; }

    int year   = ((date >> 9) & 0x7F) + 1980;
    int month  = (date >> 5) & 0x0F;
    int day    =  date & 0x1F;
    int hour   = (time_val >> 11) & 0x1F;
    int minute = (time_val >>  5) & 0x3F;
    int dsec   =  time_val & 0x1F;
    int second = dsec * 2 + (int)(tenth / 100);
    int ms     = (int)(tenth % 100) * 10;

    if (month < 1 || month > 12 || day < 1 || day > 31) {
        buf[0] = '\0'; return;
    }

    fat_time_to_str(year, month, day, hour, minute, second, ms,
                    tz_fallback_min, buf, buflen);
}

/* FAT32 アクセス日付 (日付のみ、時刻なし) → 文字列 */
static void fat32_date_to_str(uint16_t date, int tz_fallback_min,
                              char *buf, size_t buflen) {
    fat32_datetime_to_str(date, 0, 0, tz_fallback_min, buf, buflen);
}

/* ============================================================
 *  短名 (8.3) → UTF-8 文字列変換
 * ============================================================ */
static void sfn_to_str(const uint8_t *name, const uint8_t *ext,
                       uint8_t nt_res, char *buf, size_t buflen) {
    char tmp[13];
    int  pos = 0;
    int  base_lower = (nt_res & 0x08) != 0;
    int  ext_lower  = (nt_res & 0x10) != 0;

    for (int i = 0; i < 8 && name[i] != ' '; i++) {
        uint8_t ch = name[i];
        if (i == 0 && ch == DIRENT_KANJI) ch = DIRENT_DELETED;
        tmp[pos++] = base_lower ? (char)('a' + (ch - 'A') *
                     (ch >= 'A' && ch <= 'Z')) : (char)ch;
    }
    /* ext が空でなければ "." を付加 */
    int has_ext = (ext[0] != ' ');
    if (has_ext) {
        tmp[pos++] = '.';
        for (int i = 0; i < 3 && ext[i] != ' '; i++) {
            uint8_t ch = ext[i];
            tmp[pos++] = ext_lower ? (char)('a' + (ch - 'A') *
                         (ch >= 'A' && ch <= 'Z')) : (char)ch;
        }
    }
    tmp[pos] = '\0';
    snprintf(buf, buflen, "%s", tmp);
}

/* ============================================================
 *  LFN バッファから UTF-8 ファイル名を組み立てる
 * ============================================================ */
static void assemble_lfn(uint16_t lfn_buf[][LFN_CHARS_PER_ENTRY],
                         int n_lfn, char *out, size_t out_sz) {
    uint8_t utf16[LFN_MAX_CHARS * 2];
    int     total_chars = 0;

    for (int e = 0; e < n_lfn; e++) {
        for (int c = 0; c < LFN_CHARS_PER_ENTRY; c++) {
            uint16_t ch = lfn_buf[e][c];
            /* 0x0000 または 0xFFFF はパディング → 終端 */
            if (ch == 0x0000 || ch == 0xFFFF) goto done;
            if ((size_t)(total_chars * 2 + 2) >= sizeof(utf16)) goto done;
            utf16[total_chars * 2]     = (uint8_t)(ch & 0xFF);
            utf16[total_chars * 2 + 1] = (uint8_t)(ch >> 8);
            total_chars++;
        }
    }
done:
    if (total_chars > 0)
        fat_utf16le_to_utf8(utf16, (size_t)total_chars * 2, out, out_sz);
    else
        out[0] = '\0';
}

/* ============================================================
 *  ディレクトリ走査 (Pass 1 / Pass 2 共用)
 * ============================================================ */
static void walk_dir(fat_vol_t *vol, uint32_t dir_cluster,
                     ScanPass pass, CsvCtx *ctx, int depth) {
    if (depth > MAX_WALK_DEPTH) {
        fprintf(stderr, "[警告] 再帰深度超過 cluster=%u\n", dir_cluster);
        return;
    }
    if (dir_cluster < 2) return;

    uint64_t dir_size = 0;
    uint8_t *dir_data = read_cluster_chain(vol, dir_cluster, &dir_size);
    if (!dir_data || dir_size < 32) { free(dir_data); return; }

    /* LFN 蓄積バッファ */
    uint16_t lfn_buf[LFN_MAX_ENTRIES][LFN_CHARS_PER_ENTRY];
    int      lfn_count    = 0;    /* 蓄積済み LFN エントリ数 */
    int      lfn_deleted  = 0;    /* LFN が削除済みかどうか */
    uint8_t  lfn_checksum = 0;    /* LFN の checksum (短名との一致確認) */

    size_t pos = 0;
    while (pos + 32 <= dir_size) {
        const FAT32_DIRENT *de = (const FAT32_DIRENT *)(dir_data + pos);

        /* ディレクトリ終端 */
        if (de->name[0] == DIRENT_END) break;

        /* LFN エントリ */
        if (de->attr == ATTR_LFN) {
            const FAT32_LFN *lfn = (const FAT32_LFN *)(dir_data + pos);
            int is_del = (lfn->order == DIRENT_DELETED);
            int order_raw = is_del ? 0 : (lfn->order & 0x3F);

            if (!is_del && order_raw >= 1 && order_raw <= LFN_MAX_ENTRIES) {
                int idx = order_raw - 1;
                /* order の降順で並ぶため idx が若いほど後方に置かれる文字 */
                memcpy(lfn_buf[idx], lfn->name1, sizeof(lfn->name1));
                memcpy(lfn_buf[idx] + 5,  lfn->name2, sizeof(lfn->name2));
                memcpy(lfn_buf[idx] + 11, lfn->name3, sizeof(lfn->name3));
                if (lfn->order & 0x40) {
                    /* 最終エントリ (= LFN セット全体の先頭エントリ) */
                    lfn_count    = order_raw;
                    lfn_checksum = lfn->checksum;
                    lfn_deleted  = 0;
                }
            } else {
                /* 削除済み LFN: 短名エントリも削除済みのはず */
                lfn_deleted = 1;
                lfn_count   = 0;
            }
            pos += 32;
            continue;
        }

        /* ボリュームラベルエントリをスキップ (ディレクトリでも削除でもない) */
        if ((de->attr & ATTR_VOLUME_ID) && !(de->attr & ATTR_DIRECTORY)) {
            lfn_count = 0;
            pos += 32;
            continue;
        }

        /* 短名エントリ (通常ファイル / ディレクトリ) */
        int is_deleted = (de->name[0] == DIRENT_DELETED);

        /* "." ".." はスキップ */
        if (!is_deleted &&
            (de->name[0] == '.' &&
             (de->name[1] == ' ' || de->name[1] == '.'))) {
            lfn_count = 0;
            pos += 32;
            continue;
        }

        /* クラスタ番号 (28bit): cluster_high:cluster_low */
        uint32_t first_clus =
            ((uint32_t)de->cluster_high << 16) | de->cluster_low;
        first_clus &= FAT32_MASK;

        int is_dir   = (de->attr & ATTR_DIRECTORY) != 0;
        uint64_t fsize = is_dir ? 0 : (uint64_t)de->file_size;

        /* ── ファイル名決定 ──
         *   LFN があり checksum が一致 → LFN 優先
         *   それ以外                   → 短名 (8.3)
         */
        char name[MAX_NAME_UTF8] = "";

        int use_lfn = 0;
        if (!is_deleted && lfn_count > 0 && lfn_checksum != 0) {
            /* 短名の checksum を計算して LFN の checksum と照合 */
            uint8_t chk = 0;
            for (int i = 0; i < 11; i++) {
                uint8_t b = (i < 8) ? de->name[i] : de->ext[i - 8];
                chk = (uint8_t)(((chk & 1) ? 0x80 : 0) + (chk >> 1) + b);
            }
            if (chk == lfn_checksum) {
                assemble_lfn(lfn_buf, lfn_count, name, sizeof(name));
                use_lfn = (name[0] != '\0');
            }
        } else if (is_deleted && lfn_deleted) {
            /* 削除済みエントリの LFN は checksum 照合できないが
             * 直前の lfn_buf にデータが残っていれば使用を試みる */
            if (lfn_count > 0) {
                assemble_lfn(lfn_buf, lfn_count, name, sizeof(name));
                use_lfn = (name[0] != '\0');
            }
        }

        if (!use_lfn) {
            /* 短名フォールバック */
            uint8_t first = is_deleted ? (uint8_t)'?' : de->name[0];
            uint8_t tmp_name[8];
            memcpy(tmp_name, de->name, 8);
            if (is_deleted) tmp_name[0] = '?';
            sfn_to_str(tmp_name, de->ext, de->nt_res, name, sizeof(name));
            (void)first;
        }

        if (name[0] == '\0') { lfn_count = 0; pos += 32; continue; }

        /* ────────── Pass 1: ディレクトリテーブル構築 ────────── */
        if (pass == PASS1) {
            if (is_dir && !is_deleted && first_clus >= 2) {
                int already = (fat_dir_find(vol, first_clus) != NULL);
                fat_dir_add(vol, first_clus, dir_cluster, name);
                if (!already)
                    walk_dir(vol, first_clus, pass, ctx, depth + 1);
            }

        /* ────────── Pass 2: CSV 出力 ────────── */
        } else {
            char ts_mod[40], ts_acc[40], ts_cre[40];

            fat32_datetime_to_str(de->write_date,  de->write_time,  0,
                                  vol->tz_fallback_min, ts_mod, sizeof(ts_mod));
            fat32_date_to_str    (de->access_date, vol->tz_fallback_min,
                                  ts_acc, sizeof(ts_acc));
            fat32_datetime_to_str(de->create_date, de->create_time,
                                  de->create_time_tenth,
                                  vol->tz_fallback_min, ts_cre, sizeof(ts_cre));

            const char *status = is_deleted ? "DELETED" : "IN_USE";
            const char *type   = is_dir     ? "DIR"     : "FILE";

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

            if (is_deleted) ctx->deleted++; else ctx->in_use++;
            ctx->total++;

            if (is_dir && !is_deleted && first_clus >= 2)
                walk_dir(vol, first_clus, pass, ctx, depth + 1);
        }

        /* LFN バッファをリセット */
        lfn_count   = 0;
        lfn_deleted = 0;
        pos += 32;
    }

    free(dir_data);
}

/* ============================================================
 *  シグネチャ判定
 * ============================================================ */
int fat32_is_signature(const uint8_t *sector0) {
    const FAT32_BPB *bpb = (const FAT32_BPB *)sector0;
    if (memcmp(bpb->fs_type, FAT32_FS_TYPE, 8) == 0) return 1;
    /* boot_sig が 0x29 でない古いフォーマット用フォールバック判定 */
    if (bpb->root_entry_count == 0 && bpb->fat_size_32 != 0) return 1;
    return 0;
}

/* ============================================================
 *  ボリュームデコード本体
 * ============================================================ */
int fat32_decode_volume(int fd, const char *dev,
                        const char *csv_path, int tz_fallback_min) {
    fat_vol_t vol;
    memset(&vol, 0, sizeof(vol));
    vol.fd              = fd;
    vol.tz_fallback_min = tz_fallback_min;

    /* ── BPB 読み込み ── */
    FAT32_BPB bpb;
    if (fat_full_pread(fd, &bpb, sizeof(bpb), 0) != sizeof(bpb)) {
        fprintf(stderr, "Error: BPB 読み取り失敗\n");
        return 1;
    }

    /* ── ボリューム情報を計算 ── */
    vol.bytes_per_sec  = bpb.bytes_per_sector;
    vol.bytes_per_clus = (uint32_t)vol.bytes_per_sec * bpb.sectors_per_cluster;
    vol.root_cluster   = bpb.root_cluster;

    /* FAT 先頭: reserved_sectors セクタ目 */
    uint64_t fat_start_sec = bpb.reserved_sectors;
    vol.fat_offset_b = fat_start_sec * vol.bytes_per_sec;

    /* クラスタヒープ先頭: reserved + num_fats × fat_size_32 セクタ目 */
    uint64_t data_start_sec =
        (uint64_t)bpb.reserved_sectors +
        (uint64_t)bpb.num_fats * bpb.fat_size_32;
    vol.heap_offset_b = data_start_sec * vol.bytes_per_sec;

    /* クラスタ総数 */
    uint64_t total_sec = bpb.total_sectors_32
                       ? bpb.total_sectors_32 : bpb.total_sectors_16;
    vol.cluster_count = (uint32_t)((total_sec - data_start_sec) /
                                   bpb.sectors_per_cluster);

    /* ボリュームシリアル番号 */
    uint32_t vol_id = bpb.volume_id;

    /* ボリュームラベル (ANSI → そのまま表示) */
    char vol_label[12] = "";
    memcpy(vol_label, bpb.volume_label, 11);
    vol_label[11] = '\0';
    for (int i = 10; i >= 0 && vol_label[i] == ' '; i--) vol_label[i] = '\0';

    fprintf(stderr, "# デバイス              : %s\n", dev);
    fprintf(stderr, "# FS 種別               : FAT32\n");
    fprintf(stderr, "# シリアル番号          : %04X-%04X\n",
            vol_id >> 16, vol_id & 0xFFFF);
    fprintf(stderr, "# ボリュームラベル      : %s\n", vol_label);
    fprintf(stderr, "# セクタサイズ          : %u B\n",   vol.bytes_per_sec);
    fprintf(stderr, "# クラスタサイズ        : %u B\n",   vol.bytes_per_clus);
    fprintf(stderr, "# クラスタ総数          : %u\n",     vol.cluster_count);
    fprintf(stderr, "# ルートクラスタ        : %u\n",     vol.root_cluster);
    fprintf(stderr, "# FAT 開始オフセット    : 0x%llx\n",
            (unsigned long long)vol.fat_offset_b);
    fprintf(stderr, "# クラスタヒープ開始    : 0x%llx\n",
            (unsigned long long)vol.heap_offset_b);

    if (vol.tz_fallback_min != TZ_NOT_SET) {
        int sign  = (vol.tz_fallback_min >= 0) ? 1 : -1;
        int abs_m = vol.tz_fallback_min * sign;
        fprintf(stderr,
                "# タイムスタンプ        : UTC 変換モード\n"
                "#   フォールバック      : %c%02d:%02d\n"
                "#   列名               : *_utc\n",
                (sign >= 0) ? '+' : '-', abs_m / 60, abs_m % 60);
    } else {
        fprintf(stderr,
                "# タイムスタンプ        : ローカル時刻のまま出力\n"
                "#   ヒント             : -tz +0900 で UTC 変換可能\n");
    }

    /* ── FAT をメモリに読み込む ── */
    vol.fat_entries = vol.cluster_count + 2;
    uint64_t fat_bytes = (uint64_t)vol.fat_entries * 4;
    vol.fat = malloc(fat_bytes);
    if (!vol.fat) { perror("malloc FAT"); return 1; }

    fprintf(stderr, "# FAT 読み込み中 (%llu MB)...\n",
            (unsigned long long)((fat_bytes + (1 << 20) - 1) >> 20));
    ssize_t fat_read = fat_full_pread(fd, vol.fat, fat_bytes,
                                      (off_t)vol.fat_offset_b);
    if (fat_read < (ssize_t)fat_bytes)
        fprintf(stderr, "[警告] FAT 読み取り一部不完全 (%lld / %llu bytes)\n",
                (long long)fat_read, (unsigned long long)fat_bytes);
    fprintf(stderr, "# FAT 読み込み完了\n");

    /* ── Pass 1: ディレクトリテーブル構築 ── */
    fprintf(stderr, "# Pass 1: ディレクトリテーブル構築中...\n");
    fat_dir_add(&vol, vol.root_cluster, 0, "");   /* ルートを登録 */
    walk_dir(&vol, vol.root_cluster, PASS1, NULL, 0);
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
    walk_dir(&vol, vol.root_cluster, PASS2, &ctx, 0);

    if (csv_fp != stdout) fclose(csv_fp);

    fprintf(stderr, "# ─────────────────────────────────\n");
    fprintf(stderr, "# 処理済みエントリ: %llu\n",  (unsigned long long)ctx.total);
    fprintf(stderr, "# IN_USE          : %llu\n",  (unsigned long long)ctx.in_use);
    fprintf(stderr, "# DELETED         : %llu\n",  (unsigned long long)ctx.deleted);
    if (csv_path)
        fprintf(stderr, "# 出力ファイル    : %s\n", csv_path);

    /* クリーンアップ */
    fat_dir_table_free(&vol);
    free(vol.fat);
    return 0;
}
