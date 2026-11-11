/* evtx_chunk.c
 *
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "hex_dump.h"
#include "utf16le.h"

#include "evtx_file.h"
#include "evtx_chunk.h"
#include "evtx_record.h"
#include "evtx_binxml.h"
#include "evtx_output.h"




// functions only called in this file
static void decode_evtx_chunk_header(uint32_t chunk_base, uint8_t *chunk_buffer, uint16_t output_mode); 



static void decode_common_string_entry(uint32_t chunk_base, uint8_t *chunk_buffer, uint32_t offset, int entry_index, uint16_t output_mode) 
{
    // read the NAME ENTRY HEADER (fixed size)
    EVTX_NAME_ENTRY_HEADER *n_header = (EVTX_NAME_ENTRY_HEADER *) &chunk_buffer[offset];

    // 1. show summary line
    printf("Namestring#%02d (0x%08" PRIx32 ")\tnext_offset=0x%08" PRIx32 "\thash=0x%04" PRIx16 "\tlength=%" PRIu16 "\t", 
           entry_index, 
           chunk_base + offset, 
           n_header->next_offset, 
           n_header->hash, 
           n_header->char_count);
    
    // 2. print the UTF-16LE string
    print_name_from_offset(chunk_buffer, offset);
    printf("\n");

    // 3. if n_header.next_offset is not 0, need to jump to next_offset
    if (n_header->next_offset > 0) { 
        // call ourself
        // using -1 to indicate it's a "next_offset"
        decode_common_string_entry(chunk_base, chunk_buffer, n_header->next_offset, -1, output_mode); 
    }
}




static void decode_template_ptr_entry(uint32_t chunk_base, uint8_t *chunk_buffer, uint32_t offset, int entry_index, uint16_t output_mode) 
{

    // 1. read the TEMPLATE Definition Header (fixed size)
    EVTX_TEMPLATE_DEFINITION_HEADER *t_header = (EVTX_TEMPLATE_DEFINITION_HEADER *) &chunk_buffer[offset];

    // 2. print out summary
    printf("Template#%02d   (0x%08" PRIx32 ")\tnext_offset=0x%08" PRIx32 "\tID=0x%08" PRIx32 "\tbinxml_size=%" PRIu32 "B\n", 
           entry_index, 
           chunk_base + offset, 
           t_header->next_offset, 
           t_header->template_id, 
           t_header->data_size);

    // 3. if next_offset, recursive call ourself, but using -1 to indicate next_offset
    if (t_header->next_offset > 0) {
        // call ourself
        // using -1 to indicate it's a "next_offset"
        decode_template_ptr_entry(chunk_base, chunk_buffer, t_header->next_offset, -1, output_mode);
    }
}







 
int decode_evtx_chunk(FILE *fp, uint16_t chunk_index, uint32_t output_mode)
{
    // the absolute offset in the file, it should be 0x00001000, 0x00011000, 0x00021000, ...
    // this is the absolute starting point of this chunk in the input evtx file
    uint32_t chunk_base =
             sizeof(EVTX_FILE_HEADER) + (uint32_t)chunk_index * EVTX_CHUNK_SIZE;

    // read the whole chunk into memory
    uint8_t *chunk_buffer = malloc(EVTX_CHUNK_SIZE);
    fseek(fp, chunk_base, SEEK_SET); 
    fread(chunk_buffer, 1, EVTX_CHUNK_SIZE, fp);
    
    // the chunk header
    EVTX_CHUNK_HEADER *ch = (EVTX_CHUNK_HEADER *)chunk_buffer; 

    // verify the signature first
    if (memcmp(ch->signature, EVTX_CHUNK_SIGNATURE, sizeof(EVTX_CHUNK_SIGNATURE)) != 0) {
        fprintf(stderr, "Invalid CHUNK signature\n");
        return 1;
    }

    // decode the header: first 512 bytes 
    decode_evtx_chunk_header(chunk_base, chunk_buffer, output_mode);

    // walk through all records in this chunk, if there are records 
    if (ch->first_record_identifier > 0) { 

        // how many records in this chunk: (but how about some records are deleted?)
        uint64_t record_count = ch->last_record_identifier - ch->first_record_identifier + 1;

        // set record_base (related to chunk_base) to 1st reord, that's is 0x200
        uint32_t record_base = sizeof(EVTX_CHUNK_HEADER); 

        for (uint64_t i = 0; i < record_count; i++) { // decode each record
            // the stuct to hold the record header
            EVTX_RECORD_HEADER *rh = (EVTX_RECORD_HEADER *) &chunk_buffer[record_base]; 
    
            // call function to handle this record
            if (decode_evtx_record(chunk_base, record_base, chunk_buffer, output_mode) != 0) {
                return 3;   // wrong record found
            }
    
            // move to next record and alignment to 8 Bytes  
            record_base += ALIGN_8(rh->record_size);
    
            // check the hard limit
            if (record_base > ch->free_space_offset) {
                break;
            }
        }
    }

    return 0;
}


// decode header
static void decode_evtx_chunk_header(uint32_t chunk_base, uint8_t *chunk_buffer, uint16_t output_mode) 
{
    // the chunk header
    EVTX_CHUNK_HEADER *ch = (EVTX_CHUNK_HEADER *)chunk_buffer; 

    // byte 0 to 127 (128 bytes)
    if (IS_OUT_DEFAULT(output_mode)) {
        // get the index of this chunk just for summary line output
        uint64_t chunk_index = (chunk_base - sizeof(EVTX_FILE_HEADER)) / EVTX_CHUNK_SIZE;

        // and print out header details
        printf("%.8s#%05" PRIu64 " (0x%08" PRIx32 ")\t", 
               ch->signature, 
               chunk_index,
               chunk_base); 
        printf("record_num=%" PRIu64 "-%" PRIu64 "\t",
               ch->first_record_number,
               ch->last_record_number);
        printf("record_id=%" PRIu64 "-%" PRIu64 "\t",
               ch->first_record_identifier,
               ch->last_record_identifier);
        printf("last_offset=0x%" PRIx32 "\tfree_offset=0x%" PRIx32,
               ch->last_record_offset,
               ch->free_space_offset);
        printf("\n");
    }

    if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
        hex_dump_bytes((uint8_t *)ch, ch->header_size);
    }

    if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
        // byte 128 to 383 (256 bytes)
        // common string array: process each uint32 entry 
        for (int i = 0; i < 64; i++) {
            uint32_t string_offset = ch->string_offset_array[i];
            if (string_offset > 0) {   // if this offset is in using 
                // call function to process each
                decode_common_string_entry(chunk_base, chunk_buffer, string_offset, i, output_mode);
            }
        }
    }

    if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
        // byte 384 to 512 (128 bytes)
        // template definition array: process each uint32 entry
        for (int i = 0; i < 32; i++) {
            uint32_t template_offsets = ch->template_ptr_array[i];
            if (template_offsets > 0) { // if this offset is in using 
                // call function to process each
                decode_template_ptr_entry(chunk_base, chunk_buffer, template_offsets, i, output_mode);
            }
        }
    }

}


