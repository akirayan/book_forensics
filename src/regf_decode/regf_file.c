/* 
 * regf_file.c
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "regf_main.h"
#include "regf_file.h"
#include "regf_cell.h"
#include "regf_hbin.h"
#include "regf_keys.h"
#include "timestamp.h"




static uint32_t calc_checksum(const uint8_t *buf)
{
    uint32_t x = 0;
    const uint32_t *p = (const uint32_t *)buf;
    for (int i = 0; i < 127; i++) x ^= p[i]; 
    return x;
} 

/* verbose output */
static void verbose_regf_header(REGF_DECODE_INFO *hdi)
{
    regf_header_t *hdr = (regf_header_t *)(hdi->buffer);

    char ts[64];
    uint64_t ft = read_filetime((uint8_t *)&(hdr->last_written));
    filetime_to_str(ft, ts, sizeof(ts));

#define ONELINE 1
#ifdef ONELINE
    fprintf(stderr, "#regf\troot_cell_offset 0x%08x (0x%08x)\thbins_dada_size 0x%08x (%u bytes)\n",
          hdr->root_cell_offset,
          hdr->root_cell_offset + CELL_BASE,
          hdr->hive_bins_data_size,
          hdr->hive_bins_data_size);
#else
    fprintf(stderr, "# regf header\n");
    fprintf(stderr, "#   magic            : %.4s\n",  hdr->magic);
    fprintf(stderr, "#   primary_seqnum   : %u\n",    hdr->primary_seqnum);
    fprintf(stderr, "#   secondary_seqnum : %u\n",    hdr->secondary_seqnum);
    fprintf(stderr, "#   last_written     : %s\n",    ts);
    fprintf(stderr, "#   version          : %u.%u\n", hdr->major_version, hdr->minor_version);
    fprintf(stderr, "#   file_type_format : type=%u format=%u\n", hdr->file_type, hdr->file_format);
    fprintf(stderr, "#   root_cell_offset : 0x%08X (file: 0x%08X)\n",
            hdr->root_cell_offset, hdr->root_cell_offset + CELL_BASE);
    fprintf(stderr, "#   hbins_data_size  : 0x%08X (%u bytes)\n",
            hdr->hive_bins_data_size, hdr->hive_bins_data_size);
    fprintf(stderr, "#   checksum         : 0x%08X [%s]\n",
            hdr->checksum,
            calc_checksum((const uint8_t *)hdr) == hdr->checksum ? "OK" : "MISMATCH");
    fprintf(stderr, "#\n");
#endif



}




int decode_regf_file(REGF_DECODE_INFO *rdi)
{
    regf_header_t *hdr = (regf_header_t *)(rdi->buffer);

    if (CHECK_OUTMODE(rdi->output_mode, OUT_VERBOSE)) {
        verbose_regf_header(rdi);
        verbose_all_hbins(rdi);
    }

    // the buff to hold path
    char path[MAX_PATH_LEN] = "";

    // recursive calling 
    enumerate_keys(rdi, hdr->root_cell_offset, path, 0);

    return 0;
}

