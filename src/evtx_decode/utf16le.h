/*
 * utf16le.h
 *
 */

#if !defined( UTF16LE_H )
#define UTF16LE_H




#include <stdint.h>
void print_utf16le_string(uint16_t char_count, uint16_t *utf16le_data);
char *get_utf16le_string(uint16_t char_count, uint16_t *utf16le_data);

void print_name_from_offset(uint8_t *chunk_buffer, uint32_t name_offset);
char *get_name_from_offset(uint8_t *chunk_buffer, uint32_t name_offset);

#endif

