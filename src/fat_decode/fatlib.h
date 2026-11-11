/*
 * fatlib.h - FAT ファミリ (FAT32/exFAT) デコーダ共通ライブラリ
 *
 * fat32.c (FAT32/VFAT) と exfat.c (exFAT) が共有するヘルパ群を提供する。
 *   - デバイス読み込み (full_pread)
 *   - -tz 引数パース / タイムスタンプ整形
 *   - UTF-16LE → UTF-8 変換
 *   - CSV フィールド出力
 *   - ボリューム情報コンテキスト (fat_vol_t)
 *   - クラスタ→バイトオフセット変換
 *   - ディレクトリテーブル (Pass 1 で構築し Pass 2 でフルパス解決)
 */

#ifndef FATLIB_H
#define FATLIB_H

#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>

/* ============================================================
 *  共通定数
 * ============================================================ */

/* バッファ・配列上限 */
#define MAX_NAME_UTF8   768
#define MAX_PATH_UTF8   4096
#define DIR_HASH_SZ     8192
#define MAX_DEPTH       64
#define MAX_WALK_DEPTH  64

/* -tz 未指定を示す sentinel */
#define TZ_NOT_SET      INT_MIN

/* ============================================================
 *  ディレクトリテーブル
 *
 *  Pass 1 で全ディレクトリ (cluster → 親cluster + 名前) を登録し、
 *  Pass 2 の fat_build_fullpath でフルパスを復元する。
 * ============================================================ */
typedef struct DirNode {
    uint32_t cluster;
    uint32_t parent_cluster;
    char     name[MAX_NAME_UTF8];
    struct DirNode *next;
} DirNode;

/* ============================================================
 *  ボリューム情報コンテキスト
 *
 *  グローバル変数を使わず、FAT32/exFAT の各デコーダが
 *  自分のボリューム状態をこの構造体にまとめて持ち回る。
 * ============================================================ */
typedef struct {
    int      fd;                    /* 対象デバイス/イメージの fd */
    uint32_t bytes_per_sec;         /* セクタサイズ */
    uint32_t bytes_per_clus;        /* クラスタサイズ */
    uint64_t fat_offset_b;          /* FAT 先頭のバイトオフセット */
    uint64_t heap_offset_b;         /* クラスタヒープ先頭 (クラスタ 2) */
    uint32_t root_cluster;          /* ルートディレクトリ先頭クラスタ */
    uint32_t cluster_count;         /* クラスタ総数 */

    uint32_t *fat;                  /* FAT をメモリに読み込んだ配列 (生の値) */
    uint32_t  fat_entries;          /* fat[] の要素数 */

    int       tz_fallback_min;      /* -tz フォールバック (分)。TZ_NOT_SET=未指定 */

    DirNode  *dir_hash[DIR_HASH_SZ];/* ディレクトリテーブル */
} fat_vol_t;

/* ============================================================
 *  CSV 出力の集計コンテキストと走査パス種別 (FAT32/exFAT 共通)
 * ============================================================ */
typedef struct { FILE *fp; uint64_t in_use, deleted, total; } CsvCtx;
typedef enum { PASS1, PASS2 } ScanPass;

/* ============================================================
 *  純粋ヘルパ (ボリューム状態に依存しない)
 * ============================================================ */

/* count バイトを確実に読む pread ラッパ */
ssize_t fat_full_pread(int fd, void *buf, size_t count, off_t offset);

/* -tz 引数パース: "+0900" / "-0500" / "+0530" → 分単位の整数。成功 0, 失敗 -1 */
int fat_parse_tz_offset(const char *s, int *out_min);

/* UTF-16LE → UTF-8 変換 (iconv)。成功 0, 失敗 -1 */
int fat_utf16le_to_utf8(const uint8_t *src, size_t src_bytes,
                        char *dst, size_t dst_bytes);

/* CSV フィールド出力 ("..." で括り、内部の " は "" にエスケープ) */
void fat_csv_write_field(FILE *fp, const char *s);

/*
 * 分解済みローカル時刻 → 文字列 "%04d-%02d-%02d %02d:%02d:%02d.%03d"
 *
 * eff_offset_min:
 *   TZ_NOT_SET 以外 → その UTC オフセット (分) で UTC 変換して出力
 *   TZ_NOT_SET      → ローカル時刻のまま出力
 *
 * FAT32/exFAT のタイムスタンプを各モジュールがビット展開し、
 * 実効 UTC オフセットを決めた上でこの関数を呼ぶ。
 */
void fat_time_to_str(int year, int month, int day,
                     int hour, int minute, int second, int ms,
                     int eff_offset_min, char *buf, size_t buflen);

/* ============================================================
 *  ボリューム依存ヘルパ
 * ============================================================ */

/* クラスタ番号 → パーティション内バイトオフセット (クラスタは 2 始まり) */
off_t fat_cluster_to_byte(const fat_vol_t *vol, uint32_t cluster);

/* ディレクトリテーブル検索・登録 */
DirNode *fat_dir_find(fat_vol_t *vol, uint32_t cluster);
void     fat_dir_add(fat_vol_t *vol, uint32_t cluster,
                     uint32_t parent_cluster, const char *name);

/* parent_cluster + leaf 名から "/a/b/leaf" 形式のフルパスを構築 */
void fat_build_fullpath(fat_vol_t *vol, uint32_t parent_cluster,
                        const char *leaf, char *out, size_t out_size);

/* ディレクトリテーブルの全ノードを解放 */
void fat_dir_table_free(fat_vol_t *vol);

#endif /* FATLIB_H */
