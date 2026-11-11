
#if !defined( EVTX_BINXML_H )
#define EVTX_BINXML_H


#include <stdint.h>
typedef struct {
    uint8_t *chunk_buffer;    // For Name Offset (Chunk-wide)
    uint8_t *data_ptr;        // Current position in the stream
    uint8_t *end_ptr;         // data_ptr + binxml_size (The boundary)
} BinXmlContext;

#include "evtx_xmltree.h"


void decode_binxml(uint8_t *chunk_buffer, uint32_t binxml_offset, uint32_t binxml_size, uint32_t output_mode, XML_TREE *xtree);

const char* get_value_type_name(uint8_t value_type);


#endif
 


