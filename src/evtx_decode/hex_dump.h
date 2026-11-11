
#if !defined( HEX_DUMP_H )
#define HEX_DUMP_H


#include <stdint.h>
void hex_dump_bytes(const uint8_t *ptr, uint32_t size);

void hex_dump_file(FILE *fp, uint32_t offset, uint32_t size);


#endif


