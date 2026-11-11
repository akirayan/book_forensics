/* evtx_file.h
 *
 *
 */

#if !defined( EVTX_FILE_H )
#define EVTX_FILE_H


#include <stdint.h>
#include <inttypes.h>



#define EVTX_FILE_SIGNATURE "ElfFile"

#pragma pack(push, 1)
typedef struct _EVTX_FILE_HEADER {
    uint8_t  signature[8];       // 0x00  "ElfFile\x00"
    uint64_t first_chunk_number; // 0x08  
    uint64_t last_chunk_number;  // 0x10
    uint64_t next_record_id;     // 0x18
    uint32_t header_size;        // 0x20 (128)
    uint16_t minor_version;      // 0x24 (2)
    uint16_t major_version;      // 0x26 (3)
    uint16_t header_block_size;  // 0x28 (4096) or call this as chunk_data_offset
    uint16_t chunk_count;        // 0x2A 
    uint8_t  unused[76];         // 0x2C 
    uint32_t flags;              // 0x78  (0x01: Dirty, 0x02: Full)
    uint32_t checksum;           // 0x7C  signatureからflagsまでの120バイトのCRC32
    uint8_t  padding[3968];      // 0x80
} EVTX_FILE_HEADER;
#pragma pack(pop)

int decode_evtx_file(FILE *fp, uint32_t output_mode);

#endif /* !defined( EVTX_FILE_H ) */
