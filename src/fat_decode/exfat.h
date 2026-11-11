/*
 * exfat.h - exFAT パーサ
 */

#ifndef EXFAT_H
#define EXFAT_H

#include <stdint.h>

/*
 * sector0 (先頭 512 バイト以上) が exFAT ボリュームかどうか判定する。
 *   FS Name (オフセット 0x03, 8 バイト) == "EXFAT   " なら exFAT。
 * exFAT とみなせれば 1、それ以外は 0。
 */
int exfat_is_signature(const uint8_t *sector0);

/*
 * exFAT ボリュームをデコードし CSV を出力する。
 *   fd               : オープン済みの対象デバイス/イメージ (本関数では close しない)
 *   dev              : 情報表示用のデバイス名
 *   csv_path         : 出力 CSV パス (NULL なら stdout)
 *   tz_fallback_min  : -tz フォールバック (分)。TZ_NOT_SET で未指定
 * 戻り値: 0 成功、非 0 失敗 (= プロセス終了コード)
 */
int exfat_decode_volume(int fd, const char *dev,
                        const char *csv_path, int tz_fallback_min);

#endif /* EXFAT_H */
