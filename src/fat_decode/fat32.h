/*
 * fat32.h - FAT32/VFAT パーサ
 */

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

/*
 * sector0 (先頭 512 バイト以上) が FAT32 ボリュームかどうか判定する。
 *   fs_type == "FAT32   " が最も確実。
 *   古いフォーマット用フォールバック: root_entry_count==0 かつ fat_size_32!=0
 * FAT32 とみなせれば 1、それ以外は 0。
 */
int fat32_is_signature(const uint8_t *sector0);

/*
 * FAT32 ボリュームをデコードし CSV を出力する。
 *   fd               : オープン済みの対象デバイス/イメージ (本関数では close しない)
 *   dev              : 情報表示用のデバイス名
 *   csv_path         : 出力 CSV パス (NULL なら stdout)
 *   tz_fallback_min  : -tz フォールバック (分)。TZ_NOT_SET で未指定
 * 戻り値: 0 成功、非 0 失敗 (= プロセス終了コード)
 */
int fat32_decode_volume(int fd, const char *dev,
                        const char *csv_path, int tz_fallback_min);

#endif /* FAT32_H */
