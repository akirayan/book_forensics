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

    // decode the evtx file header first
    if (decode_evtx_file_header(fp, &fh, output_mode) != 0) {
        return 1;
    }

    // Decide how many chunks to walk.
    //
    // We must NOT trust fh.chunk_count here. When a log is still in use its
    // header is often "dirty" (flags & 0x01) and the chunk_count field is left
    // stale -- a Security.evtx copied from a live Windows host typically shows
    // chunk_count == 1 even though it holds dozens of chunks. Trusting that value
    // would silently decode only the first 64 KiB and drop the rest of the log.
    //
    // Instead, scan every fixed 64 KiB slot from the end of the file header to
    // EOF and decode each slot that starts with a valid "ElfChnk" signature,
    // stopping at the first slot that is not a chunk (or does not fully fit).
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);

    for (uint32_t i = 0; ; i++) {
        long chunk_base =
            (long)sizeof(EVTX_FILE_HEADER) + (long)i * EVTX_CHUNK_SIZE;

        // a full chunk must fit inside the file
        if (chunk_base + EVTX_CHUNK_SIZE > file_size) {
            break;
        }

        // peek the slot's signature; stop at the first non-chunk slot
        char sig[sizeof(EVTX_CHUNK_SIGNATURE)];
        fseek(fp, chunk_base, SEEK_SET);
        if (fread(sig, 1, sizeof(sig), fp) != sizeof(sig)) {
            break;
        }
        if (memcmp(sig, EVTX_CHUNK_SIGNATURE, sizeof(EVTX_CHUNK_SIGNATURE)) != 0) {
            break;
        }

        decode_evtx_chunk(fp, i, output_mode);
    }

    return 0;
}
