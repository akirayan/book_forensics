
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>


#define BYTES_PER_LINE  16

void hex_dump_bytes(const uint8_t *ptr, uint32_t size)
{
    if (size == 0) {
        printf("    [hex_dump_bytes] size = 0, nothing to dump\n");
        return;
    }

    uint32_t offset = 0;

    while (offset < size) {
        uint32_t remaining = size - offset;
        uint32_t line_bytes =
            remaining > BYTES_PER_LINE ? BYTES_PER_LINE : remaining;

        /* hex part */
        printf("%08x  ", offset);
        for (uint32_t i = 0; i < BYTES_PER_LINE; i++) {
            if (i < line_bytes) {
                printf("%02x ", ptr[offset + i]);
            } else {
                printf("   ");
            }
            if (i == (BYTES_PER_LINE / 2 - 1)) printf(" ");
        }

        /* ASCII part */
        printf(" |");
        for (uint32_t i = 0; i < line_bytes; i++) {
            uint8_t c = ptr[offset + i];
            printf("%c", isprint(c) ? c : '.');
            if (i == (BYTES_PER_LINE / 2 - 1)) printf(" ");
        }
        printf("|\n");

        offset += line_bytes;
    }
}


void hex_dump_file(FILE *fp,
                   uint32_t offset,
                   uint32_t size)
{
    if (size == 0) {
        printf("    [hex_dump_file] size = 0, nothing to dump\n");
        return;
    }

    if (fseek(fp, offset, SEEK_SET) != 0) {
        perror("fseek(hex_dump_file)");
        return;
    }

    uint8_t *buf = malloc(size);
    if (!buf) {
        perror("malloc(hex_dump_file)");
        return;
    }

    size_t n = fread(buf, 1, size, fp);
    if (n != size) {
        printf("    [hex_dump_file] fread failed or EOF "
               "(expected %u, got %zu)\n", size, n);
        free(buf);
        return;
    }

    hex_dump_bytes(buf, size);

    free(buf);
}




