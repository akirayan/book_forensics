/* 
 * regf_cell.c
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "regf_main.h"
#include "regf_file.h"
#include "regf_cell.h"
#include "timestamp.h"


/* セルオフセット → バッファ内ポインタ（keys/value 共有）。 */
uint8_t *regf_cell_ptr(const REGF_DECODE_INFO *rdi, uint32_t offset)
{
    if (offset == 0xFFFFFFFF) return NULL;
    uint64_t file_off = (uint64_t)offset + CELL_BASE;
    if (file_off + 4 > rdi->length) return NULL;
    return rdi->buffer + file_off;
}




static void verbose_nk_cell(const nk_cell_t *nk) 
{
    fprintf(stderr, "    [NK] Key Name      : %.*s\n", nk->name_length, nk->name);

    fprintf(stderr, "    [NK] parent_offset : %u\n", nk->parent_offset);

    fprintf(stderr, "    [NK] subkey_count  : %u\n", nk->subkey_count);
    if (nk->subkey_count > 0) {
        fprintf(stderr, "    [NK]     subkey_list_offset: 0x%08x (%08u)\n",
                                               nk->subkey_list_offset,
                                               nk->subkey_list_offset);
    }

    fprintf(stderr, "    [NK] value_count   : %u\n", nk->value_count);
    if (nk->value_count > 0) {
        fprintf(stderr, "    [NK]     value_list_offset: 0x%08x (%08u)\n",
                                               nk->value_list_offset,
                                               nk->value_list_offset);
    }
}



static void verbose_cell(const cell_header_t *cell_hdr, uint32_t cell_ptr)
{
    int32_t cell_size_raw = cell_hdr->cell_size;
    uint32_t abs_size = (cell_size_raw < 0) ? -cell_size_raw : cell_size_raw;

    // Fix: Declare sig as a local array (3 bytes: 2 for chars + 1 for null terminator)
    char sig[3]; 

    sig[0] = isprint((unsigned char)cell_hdr->magic[0]) ? cell_hdr->magic[0] : '.';
    sig[1] = isprint((unsigned char)cell_hdr->magic[1]) ? cell_hdr->magic[1] : '.';
    sig[2] = '\0'; // Properly null-terminate

    if (cell_size_raw < 0) {
        // Use %s to print the null-terminated string
        fprintf(stderr, "#cell %s\tOffset 0x%04x\tSize %-5d bytes\n", sig, cell_ptr, abs_size);
    } else {
        fprintf(stderr, "#free %s\tOffset 0x%04x\tSize %-5d bytes\n", sig, cell_ptr, abs_size);
    }
}


void verbose_all_cells(const hbin_header_t *hbin_hdr, uint8_t *regf_buffer, uint32_t regf_offset)
{
    uint32_t cell_ptr = sizeof(hbin_header_t); // Start after the 32-byte hbin header
    uint32_t current_bin_size = hbin_hdr->hbin_size;

    while (cell_ptr < current_bin_size) {
        // Pointer to the start of the cell
        //uint8_t *cell_start = (uint8_t *)hbin_hdr + cell_ptr;

        // get the CELL_HEADER
        const cell_header_t *cell_hdr = (const cell_header_t *)(regf_buffer + regf_offset + cell_ptr);

        // Read the 4-byte signed size
        int32_t cell_size_raw = cell_hdr->cell_size;
        uint32_t abs_size = (cell_size_raw < 0) ? -cell_size_raw : cell_size_raw;

        // 0. Safety check for corruption
        if (abs_size == 0 || (cell_ptr + abs_size) > current_bin_size) {
            fprintf(stderr, "  [!] Invalid cell size at bin offset 0x%X\n", cell_ptr);
            break;
        }

        // 1. Alignment Sanity Check
        if (abs_size % 8 != 0) {
            fprintf(stderr, "  [!] Alignment Error: Cell at 0x%X has non-8-byte size %u\n",
                    cell_ptr, abs_size);
            break;
        }

        // 2. Minimum Size Check
        // A cell header is 4 bytes, and the smallest possible data is 4 bytes.
        // Therefore, no cell should ever be smaller than 8 bytes.
        if (abs_size < 8) {
            fprintf(stderr, "  [!] Size Error: Cell at 0x%X is too small (%u bytes)\n",
                    cell_ptr, abs_size);
            break;
        }

        // call function to show this cell
        verbose_cell(cell_hdr, cell_ptr);

        // Move to the next cell
        cell_ptr += abs_size;
    }
}


