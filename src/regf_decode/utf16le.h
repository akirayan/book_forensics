/*
 * utf16le.h
 *
 */

#if !defined( UTF16LE_H )
#define UTF16LE_H



void print_utf16le_string(uint16_t char_count, uint16_t *utf16le_data);
char *get_utf16le_string(uint16_t char_count, uint16_t *utf16le_data);

void print_name_from_offset(uint8_t *chunk_buffer, uint32_t name_offset);
char *get_name_from_offset(uint8_t *chunk_buffer, uint32_t name_offset);

size_t utf16le_to_utf8(const uint16_t *src, uint16_t char_count, char *dst, size_t dst_size);

#endif

