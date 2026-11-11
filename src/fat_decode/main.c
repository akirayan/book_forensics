/*
 * main.c - fat_decode: FAT32/VFAT・exFAT 統合デコーダ
 *
 * パーティション先頭の VBR を読み、OEM ID / FS 種別で FAT32 か exFAT を
 * 判定し、対応するパーサ (fat32.c / exfat.c) に振り分ける。
 * CSV 列順・-tz の挙動・削除エントリ出力は旧 fat32_decode / exfat_decode と同一。
 *
 * 列順:
 *   cluster_no, parent_cluster_no, status, type,
 *   modified_tm[_utc], accessed_tm[_utc], created_tm[_utc],
 *   filename, fullpath, file_size
 *
 * 実行:
 *   sudo ./fat_decode /dev/sda1
 *   sudo ./fat_decode /dev/sda1 out.csv
 *   sudo ./fat_decode /dev/sda1 out.csv -tz +0900   # JST→UTC
 *   sudo ./fat_decode /dev/loop0p3 out.csv -tz -0500 # EST→UTC
 *
 * -tz の動作:
 *   FAT32 : タイムスタンプはローカル時刻。-tz 指定時のみ UTC 変換 (列名 *_utc)。
 *   exFAT : UtcOffset フィールド設定済みはフィールド優先。未設定は -tz を適用。
 *
 * 注意:
 *   パーティションデバイスを直接指定する (ディスク全体デバイスは不可)。
 *   マウント中でも読み取り専用アクセスは可能。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>

#include "fatlib.h"
#include "fat32.h"
#include "exfat.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "使い方: %s <パーティションデバイス> [出力CSV] [-tz ±HHMM]\n"
        "\n"
        "  <デバイス>  パーティションデバイスを指定 (例: /dev/sda1, /dev/loop0p3)\n"
        "  [出力CSV]   省略時は stdout に出力\n"
        "  -tz ±HHMM   タイムスタンプのフォールバック UTC オフセット\n"
        "              FAT32: 指定時すべて UTC 変換 (列名 *_utc)。\n"
        "              exFAT: UtcOffset フィールドが設定済みのエントリはフィールド優先。\n"
        "                     未設定のエントリにこの値を適用。\n"
        "\n"
        "  FAT32 / exFAT は VBR のシグネチャで自動判定する。\n"
        "\n"
        "  例: sudo %s /dev/sda1\n"
        "      sudo %s /dev/loop0p1 out.csv\n"
        "      sudo %s /dev/loop0p3 out.csv -tz +0900\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    /* ── 引数パース ──
     *   fat_decode <device> [output.csv] [-tz ±HHMM]
     *   順序は任意。非オプション引数 1 個目=デバイス、2 個目=CSV。
     */
    const char *dev      = NULL;
    const char *csv_path = NULL;
    int tz_fallback_min  = TZ_NOT_SET;
    int non_opt          = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-tz") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -tz の後にオフセット値が必要です "
                                "(例: -tz +0900)\n");
                return 1;
            }
            if (fat_parse_tz_offset(argv[i + 1], &tz_fallback_min) != 0) {
                fprintf(stderr,
                        "Error: -tz の値が無効です: '%s'\n"
                        "  形式: +HHMM または -HHMM "
                        "(例: +0900, -0500, +0530)\n",
                        argv[i + 1]);
                return 1;
            }
            i++;    /* -tz の引数をスキップ */
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: 不明なオプション: %s\n", argv[i]);
            return 1;
        } else {
            if      (non_opt == 0) { dev      = argv[i]; non_opt++; }
            else if (non_opt == 1) { csv_path = argv[i]; non_opt++; }
            else { fprintf(stderr, "Error: 引数が多すぎます\n"); return 1; }
        }
    }

    if (!dev) {
        usage(argv[0]);
        return 1;
    }

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: '%s' を開けません: %s\n",
                dev, strerror(errno));
        return 1;
    }

    /* ── VBR 先頭セクタを読んで FS 種別を判定 ── */
    uint8_t sector0[512];
    if (fat_full_pread(fd, sector0, sizeof(sector0), 0) != (ssize_t)sizeof(sector0)) {
        fprintf(stderr, "Error: VBR 読み取り失敗\n");
        close(fd);
        return 1;
    }

    int rc;
    if (exfat_is_signature(sector0)) {
        rc = exfat_decode_volume(fd, dev, csv_path, tz_fallback_min);
    } else if (fat32_is_signature(sector0)) {
        rc = fat32_decode_volume(fd, dev, csv_path, tz_fallback_min);
    } else {
        fprintf(stderr,
                "Error: FAT32 / exFAT のシグネチャが見つかりません\n"
                "  OEM/FS ID: '%.8s' (0x03)  '%.8s' (0x52)\n"
                "  ヒント: パーティションデバイスを指定しているか確認\n"
                "          (例: /dev/sda1 であって /dev/sda ではない)\n"
                "          losetup -P でパーティションループデバイスを作成\n",
                (char *)(sector0 + 3), (char *)(sector0 + 0x52));
        close(fd);
        return 1;
    }

    close(fd);
    return rc;
}
