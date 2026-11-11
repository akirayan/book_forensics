/* evtx_file.c
 *
 *
 */


#include <stdio.h>
#include <string.h>

#include "evtx_file.h"
#include "evtx_chunk.h"
#include "hex_dump.h"
#include "evtx_output.h"

// verify and decode the evtx file header
static int decode_evtx_file_header(FILE *fp, EVTX_FILE_HEADER *fh, int output_mode)
{

    // verify the signature first
    if (memcmp(fh->signature, EVTX_FILE_SIGNATURE, sizeof(EVTX_FILE_SIGNATURE)) == 0) {
        if (IS_OUT_DEFAULT(output_mode)) {

            // print the contents of head
            printf("%.8s", fh->signature);
            printf("\t      version=%u.%u", fh->major_version, fh->minor_version);
            printf("\tchunk=%" PRIu64 "-%" PRIu64 "", fh->first_chunk_number, fh->last_chunk_number);
            printf("\tchunk_counts=%" PRIu16 "", fh->chunk_count);
            //printf("\tchunk_offset=0x%08" PRIx16 "", fh->header_block_size);
            printf("\tnext_record_id=%" PRIu64 "", fh->next_record_id);
            printf("\tflags=0x%02" PRIx32 "", fh->flags);
            { // flags as text
                char ftext[8]; 
                switch (fh->flags) {
                    case 0x00: strcpy(ftext, "clean"); break;;
                    case 0x01: strcpy(ftext, "dirty"); break;;
                    case 0x02: strcpy(ftext, "full"); break;;
                    default  : strcpy(ftext, "unknown"); break;;
                }
                printf("(%s)", ftext);
            }
            //printf("\theader_size=%" PRIu32 "", fh->header_size);
            printf("\n");

        } 

        if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
            uint32_t offset = 0x00000000; // the starting of this file
            hex_dump_file(fp, offset, fh->header_size);
        }

    } else {
        fprintf(stderr, "Invalid EVTX signature\n");
        return 1;
    }

    return 0;
}


int decode_evtx_file(FILE *fp, uint32_t output_mode)
{
    // read file header
    EVTX_FILE_HEADER fh;
    fread(&fh, sizeof(fh), 1, fp);

    // decode the evtx file header then decode each chunk 
    if (decode_evtx_file_header(fp, &fh, output_mode) == 0) {
        for (uint16_t i = 0; i < fh.chunk_count; i++) {
            decode_evtx_chunk(fp, i, output_mode);
        }
        return 0;
    } else {
        return 1;
    }
}
