/* evtx_chunk.h
 *
 *
 */


#if !defined( EVTX_CHUNK_H )
#define EVTX_CHUNK_H


#include <stdint.h>
#include <inttypes.h>

#define EVTX_CHUNK_SIZE             0x10000
#define EVTX_CHUNK_SIGNATURE        "ElfChnk"


#pragma pack(push, 1)
typedef struct _EVTX_CHUNK_HEADER {
    uint8_t  signature[8];            // 0x00: "ElfChnk\x00"
    uint64_t first_record_number;     // 0x08: First Event Record Number
    uint64_t last_record_number;      // 0x10: Last Event Record Number
    uint64_t first_record_identifier; // 0x18: First Event Record ID
    uint64_t last_record_identifier;  // 0x20: Last Event Record ID
    uint32_t header_size;             // 0x28: value is 128 (0x80) bytes
    uint32_t last_record_offset;      // 0x2C: last record offset related to chunk_base
    uint32_t free_space_offset;       // 0x30: free space offset related to chunk_base
    uint32_t data_checksum;           // 0x34: CRC32 of event data
    uint8_t  unknown[64];             // 0x38: 64 bytes of unkwown empty values 
    uint32_t unknown_flags;           // 0x78: not sure
    uint32_t checksum;                // 0x7C: checksum of first 120 bytes
    uint32_t string_offset_array[64]; // 0x80: common string offset array, 64 x 4bytes = 256 bytes
    uint32_t template_ptr_array[32];  // 0x180: template definition offset array, 32 x 4bytes = 128 bytes
} EVTX_CHUNK_HEADER;
#pragma pack(pop)




#pragma pack(push, 1)
// 文字列エントリの固定ヘッダー部分
typedef struct _EVTX_NAME_ENTRY_HEADER {
    uint32_t next_offset; // ハッシュ衝突時の次要素へのオフセット
    uint16_t hash;        // 文字列のハッシュ値
    uint16_t char_count;  // 文字列の長さ（文字数）
    // この後に UTF-16LE の文字列が続く (NULL終端ではない場合がある)
} EVTX_NAME_ENTRY_HEADER;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct _EVTX_TEMPLATE_DEFINITION_HEADER {
    uint32_t next_offset;          // 0x00: link to next entry if needed 
    uint32_t template_id;          // 0x04: Template ID
    uint8_t  unknown_guid[12];     // 0x08: 12 bytes unknown or guid-like
    uint32_t data_size;            // 0x14: size of following binxml data
} EVTX_TEMPLATE_DEFINITION_HEADER;
#pragma pack(pop)


int decode_evtx_chunk(FILE *fp, uint16_t chunk_index, uint32_t output_mode);

#endif
