/* evtx_record.h
 *
 *
 */


#if !defined ( EVTX_RECORD_H )
#define EVTX_RECORD_H



#include <stdint.h>
#define ALIGN_8(x) (((x) + 7) & ~7)


#pragma pack(push, 1)
typedef struct _EVTX_RECORD_HEADER {
    uint32_t signature;            // 0x00: "\x2a\x2a\x00\x00" (unsigned int 0x00002a2a)
    uint32_t record_size;          // 0x04: size of this record
    uint64_t record_identifier;    // 0x08: Record ID (usually same as Record Num)
    uint64_t timestamp;            // 0x10: FILETIME format
            	                   // 0x18: from 0x18 (4B+4B+8B+8B=24Bytes), starting BinXML, variable size
                                   // 4Bytes after binxml: uint32_t copy_of_record_size
} EVTX_RECORD_HEADER;
#define EVTX_RECORD_SIGNATURE 0x00002a2a
#pragma pack(pop)



int decode_evtx_record(uint32_t chunk_base, uint32_t record_base, uint8_t *chunk_buffer, uint32_t output_mode);

void get_item_value_by_index(uint8_t *chunk_buffer, int index);

#endif
