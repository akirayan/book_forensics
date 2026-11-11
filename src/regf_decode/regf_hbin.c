/* 
 * regf_file.c
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "regf_main.h"
#include "regf_hbin.h"
#include "regf_cell.h"




static void verbose_hbin(const hbin_header_t *hdr, uint8_t *regf_buffer, uint32_t regf_offset)
{
    // Print the info
    fprintf(stderr, "#%.4s\t\toffset 0x%08x\tsize 0x%08x (%u bytes)\n",  
                    hdr->magic,
                    hdr->hbin_offset,
                    hdr->hbin_size, hdr->hbin_size);
    //
    // Print the info of all cells in this hbin
    //fprintf(stderr, "# all cells in this hbin (at file offset 0x%08X)\n", regf_offset);
    verbose_all_cells(hdr, regf_buffer, regf_offset);
}

void verbose_all_hbins(REGF_DECODE_INFO *rdi)
{
    regf_header_t *regf_hdr = (regf_header_t *)rdi->buffer;
    
    // The total size of the HBIN area as reported by the REGF header
    uint32_t total_hbins_size = regf_hdr->hive_bins_data_size;

    // We start at the beginning of the hive bins data (typically offset 0x1000)
    uint32_t current_data_offset = 0;

    //fprintf(stderr, "# Walking all Hive Bins (Total Data Size: 0x%08X)\n", total_hbins_size);

    while (current_data_offset < total_hbins_size) {
        // Calculate the physical address of the current HBIN in the buffer
        const hbin_header_t *hbin_hdr = (const hbin_header_t *)(rdi->buffer + CELL_BASE + current_data_offset);

        // Safety check: ensure we don't read past the buffer length
        if ((CELL_BASE + current_data_offset + sizeof(hbin_header_t)) > rdi->length) {
            fprintf(stderr, "Error: Unexpected end of file buffer.\n");
            break;
        }

        // Check for valid signature
        if (memcmp(hbin_hdr->magic, "hbin", 4) != 0) {
            fprintf(stderr, "Error: Invalid hbin magic at offset 0x%08X\n", (uint32_t)(CELL_BASE + current_data_offset));
            break;
        }

        // Print the info (modified verbose_hbin slightly to include the file offset)
        verbose_hbin(hbin_hdr, rdi->buffer, (uint32_t)(CELL_BASE + current_data_offset));

        // Validation: The hbin_offset field in the header should match our current relative offset
        if (hbin_hdr->hbin_offset != current_data_offset) {
            fprintf(stderr, "Warning: Header offset (0x%X) doesn't match current data offset (0x%X)\n",
                    hbin_hdr->hbin_offset, current_data_offset);
        }

        // Move the pointer to the start of the next HBIN
        // hbin_size includes the 32-byte header itself
        uint32_t current_bin_size = hbin_hdr->hbin_size;

        // Critical: Ensure we don't get stuck in an infinite loop if size is 0
        if (current_bin_size == 0) {
            fprintf(stderr, "Error: hbin size is 0 at offset 0x%08X. Breaking to avoid infinite loop.\n", current_data_offset);
            break;
        }

        current_data_offset += current_bin_size;
    }
}


