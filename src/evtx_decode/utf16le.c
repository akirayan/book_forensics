/*
 * utf16le.c
 */


#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <iconv.h>
#include <errno.h>

#include "utf16le.h"



void print_utf16le_string(uint16_t char_count, uint16_t *utf16le_data) {
    if (!utf16le_data || char_count == 0) return;

    // 1. Create conversion descriptor (From UTF-16LE to UTF-8)
    iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
    if (cd == (iconv_t)-1) {
        perror("iconv_open failed");
        return;
    }

    // 2. Prepare buffers
    // UTF-8 can take up to 3-4 bytes per character for Japanese
    size_t in_bytes_left = char_count * 2;
    size_t out_bytes_left = char_count * 4; 
    char *out_buffer = malloc(out_bytes_left + 1);
    if (!out_buffer) {
        iconv_close(cd);
        return;
    }

    char *in_ptr = (char *)utf16le_data;
    char *out_ptr = out_buffer;

    // 3. Perform conversion
    if (iconv(cd, &in_ptr, &in_bytes_left, &out_ptr, &out_bytes_left) == (size_t)-1) {
        // If conversion fails, fallback to simple ASCII or print error
        fprintf(stderr, "\n[Conversion Error: %d]\n", errno);
    } else {
        *out_ptr = '\0'; // Null-terminate the UTF-8 string
        printf("%s", out_buffer);
    }

    // 4. Cleanup
    free(out_buffer);
    iconv_close(cd);
}






void print_name_from_offset_FILE(FILE *fp, uint32_t chunk_base, uint32_t name_offset) {
    // 1. Move to the absolute position in the file
    // The offset is always relative to the start of the chunk (ElfChnk)
    fseek(fp, chunk_base + name_offset, SEEK_SET);

    // 2. Skip the 'Next Entry Offset' (4B) and 'Hash' (2B)
    fseek(fp, 6, SEEK_CUR);

    // 3. Read the Character Count (2B)
    uint16_t char_count;
    fread(&char_count, sizeof(uint16_t), 1, fp);

    // 4. Read the raw UTF-16 bytes
    uint16_t *name_buffer = malloc(char_count * 2);
    if (name_buffer) {
        fread(name_buffer, char_count * 2, 1, fp);
        
        // 5. Use the new successful function!
        print_utf16le_string(char_count, name_buffer);
        
        free(name_buffer);
    }
}





/// This function uses the memory-mapped chunk_buffer instead of FILE I/O
void print_name_from_offset_BUFFER(uint8_t *chunk_buffer, uint32_t name_offset) 
{
    // 1. Calculate the start address of the Namestring structure
    // name_offset is relative to the start of the chunk
    uint8_t *name_struct_ptr = chunk_buffer + name_offset;

    // 2. Access the 'Next Entry Offset' (4B) and 'Hash' (2B)
    // We skip these by offsetting the pointer by 6 bytes
    uint8_t *char_count_ptr = name_struct_ptr + 6;

    // 3. Read the Character Count (2B)
    // Use a cast to uint16_t. 
    // Note: Assuming the system is Little-Endian like the Evtx format.
    uint16_t char_count = *(uint16_t *)char_count_ptr;

    // 4. Locate the raw UTF-16 bytes
    // The name string starts immediately after the 2-byte char_count
    uint16_t *name_ptr = (uint16_t *)(char_count_ptr + 2);

    // 5. Check if the string is within the 64KB buffer boundary before printing
    // (Safety check to prevent segfaults on corrupted files)
    uint32_t string_end_offset = name_offset + 6 + 2 + (char_count * 2);
    if (string_end_offset <= 0x10000) {
        // 6. Use your existing function to print the string
        // Since the data is already in memory, no need to malloc/free!
        print_utf16le_string(char_count, name_ptr);
    } else {
        printf("[Error: Namestring at 0x%04X exceeds chunk boundary]", name_offset);
    }
}



int utf16le_to_utf8(const uint16_t *src,
                    uint16_t char_count,
                    char *dst,
                    size_t dst_size)
{
    if (!dst || dst_size == 0) return -1;

    size_t out = 0;

    for (uint16_t i = 0; i < char_count; i++) {
        uint16_t wc = src[i];

        /* ASCII fast path (most EVTX strings) */
        if (wc < 0x80) {
            if (out + 1 >= dst_size) break;
            dst[out++] = (char)wc;
        }
        /* 2-byte UTF-8 */
        else if (wc < 0x800) {
            if (out + 2 >= dst_size) break;
            dst[out++] = 0xC0 | (wc >> 6);
            dst[out++] = 0x80 | (wc & 0x3F);
        }
        /* 3-byte UTF-8 (BMP) */
        else {
            if (out + 3 >= dst_size) break;
            dst[out++] = 0xE0 | (wc >> 12);
            dst[out++] = 0x80 | ((wc >> 6) & 0x3F);
            dst[out++] = 0x80 | (wc & 0x3F);
        }
    }

    dst[out] = '\0';

    return (int)out;
}




char *get_utf16le_string(uint16_t char_count, uint16_t *utf16le_data)
{
    if (!utf16le_data || char_count == 0) return NULL;

    // 1. Create conversion descriptor (From UTF-16LE to UTF-8)
    iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
    if (cd == (iconv_t)-1) {
        perror("iconv_open failed");
        return NULL;
    }

    // 2. Prepare buffers
    // UTF-8 can take up to 3-4 bytes per character for Japanese
    size_t in_bytes_left = char_count * 2;
    size_t out_bytes_left = char_count * 4; 
    char *out_buffer = malloc(out_bytes_left + 1);
    if (!out_buffer) {
        iconv_close(cd);
        free(out_buffer);
        return NULL;
    }

    char *in_ptr = (char *)utf16le_data;
    char *out_ptr = out_buffer;

    // 3. Perform conversion
    if (iconv(cd, &in_ptr, &in_bytes_left, &out_ptr, &out_bytes_left) == (size_t)-1) {
        // If conversion fails, fallback to simple ASCII or print error
        fprintf(stderr, "\n[Conversion Error: %d]\n", errno);
    } else {
        *out_ptr = '\0'; // Null-terminate the UTF-8 string
    }

    // 4. Cleanup
    iconv_close(cd);

    // return 
    return out_buffer;
}


char *get_name_from_offset(uint8_t *chunk_buffer, uint32_t name_offset)
{
    if (!chunk_buffer || name_offset == 0)
        return NULL;

    uint8_t *p = chunk_buffer + name_offset;

    // skip next_offset (4 bytes)
    p += 4;

    // skip hash (2 bytes)
    p += 2;

    // string length (UTF-16 characters)
    uint16_t char_count = *(uint16_t *)p;
    p += 2;

    if (char_count == 0)
        return strdup("");

    // allocate UTF-8 buffer
    // worst case: 1 UTF16 → 3 UTF8 bytes
    size_t max_utf8 = char_count * 3 + 1;

    char *out = malloc(max_utf8);
    if (!out)
        return NULL;

    char *dst = out;

    uint16_t *src = (uint16_t *)p;

    for (uint16_t i = 0; i < char_count; i++)
    {
        uint16_t ch = src[i];

        if (ch < 0x80)
        {
            *dst++ = ch;
        }
        else if (ch < 0x800)
        {
            *dst++ = 0xC0 | (ch >> 6);
            *dst++ = 0x80 | (ch & 0x3F);
        }
        else
        {
            *dst++ = 0xE0 | (ch >> 12);
            *dst++ = 0x80 | ((ch >> 6) & 0x3F);
            *dst++ = 0x80 | (ch & 0x3F);
        }
    }

    *dst = '\0';

    return out;
}

char *evtx_get_name(uint8_t *chunk_buffer, uint32_t offset)
{
    return get_name_from_offset(chunk_buffer, offset);;
}


void print_name_from_offset(uint8_t *chunk_buffer, uint32_t offset)
{
    char *name = get_name_from_offset(chunk_buffer, offset);
    if (name) {
        printf("%s", name);
        free(name);
    }
}

#undef TEST_UTF16LE

#ifdef TEST_UTF16LE
int main() {
    printf("--- UTF-16LE Decoder Test ---\n");


    // Note the 0xXXXX format instead of 0xXX, 0xXX
    uint16_t test1[] = {
        0x004D, 0x0069, 0x0063, 0x0072, 0x006F, 0x0073, 0x006F, 0x0066, 0x0074
    };
    
    uint16_t test2[] = {
        0x65E5, 0x672C, 0x8A9E
    };
    
    print_utf16le_string(9, test1); // No warning!
    printf("\n");
    
    print_utf16le_string(3, test2); // No warning!
    printf("\n");


    printf("-----------------------------\n");
    return 0;
}
#endif

