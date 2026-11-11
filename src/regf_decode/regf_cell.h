/* regf_cell.h
 *
 *
 */

#if !defined( REGF_CELL_H )
#define REGF_CELL_H




void verbose_all_cells(const hbin_header_t *hbin_hdr, uint8_t *regf_buffer, uint32_t regf_offset);

/* セルオフセット（hive bins data 先頭=CELL_BASE 基準）→ バッファ内ポインタ。
 * 0xFFFFFFFF や範囲外は NULL。keys/value 両層で使う共有ヘルパ。 */
uint8_t *regf_cell_ptr(const REGF_DECODE_INFO *rdi, uint32_t offset);



#endif /* !defined( REGF_CELL_H ) */
