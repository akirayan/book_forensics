/* regf_value.h
 *
 * vk セル（レジストリ値）の解読と出力。
 */

#if !defined( REGF_VALUE_H )
#define REGF_VALUE_H

#include "regf_main.h"

/* vk セル1つを  key_path/値名|型|値  形式で出力する。
 * -d PATTERN 指定時は、マッチする値を regedit 形式でフルダンプする。 */
void process_vk_cell(REGF_DECODE_INFO *rdi, uint32_t cell_offset, const char *key_path);

#endif /* !defined( REGF_VALUE_H ) */
