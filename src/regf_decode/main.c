/* main.c
 *
 * reg_decode - Windows Registry Hive Parser
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regf_main.h"
#include "regf_file.h"

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-v] [-f] [-p PATH] [-d PATTERN] <hivefile>\n", prog); 
    fprintf(stderr, "  -f          : print full binary data on same line (no truncation)\n");
    fprintf(stderr, "  -p PATH     : output only PATH and its descendants (prefix match)\n");
    fprintf(stderr, "  -d PATTERN  : full hex dump for matching value paths (wildcards * ?)\n"); 
    fprintf(stderr, "  example: -f -p \"/ROOT/ControlSet001/Enum/USBSTOR\"\n");
    fprintf(stderr, "           -d \"*PartitionTableCache\"\n");
}

const char *check_cmd_argv(REGF_DECODE_INFO *rdi, int argc, char *argv[])
{
    uint32_t output_mode = 0;
    const char *filename = NULL;
    const char *filter_path = NULL;
    const char *dump_pattern = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            SET_OUTMODE(output_mode, OUT_VERBOSE);
        } else if (strcmp(argv[i], "-f") == 0) {
            SET_OUTMODE(output_mode, OUT_FULL_BINARY);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            SET_OUTMODE(output_mode, OUT_FILTER_PATH);
            filter_path = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return NULL;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            SET_OUTMODE(output_mode, OUT_DUMP_PATTERN);
            dump_pattern = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return NULL;
        } else {
            filename = argv[i];
        }    
    }    

    // set options 
    rdi->filename = filename;
    rdi->filter_path = filter_path;
    rdi->dump_pattern = dump_pattern;
    rdi->output_mode = output_mode;

    return filename;
}



int main(int argc, char **argv)
{
    REGF_DECODE_INFO rdi;
    const char *filename = check_cmd_argv(&rdi, argc, argv);

    if (!filename) {
        fprintf(stderr, "ERROR: no file specified\n");
        usage(argv[0]);
        return 1;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) { perror("fopen"); return 1; }

    // check the file size
    fseek(fp, 0, SEEK_END);
    rdi.length = (size_t)ftell(fp);
    rewind(fp);
    if (rdi.length < REGF_HEADER_SIZE) {
        fprintf(stderr, "%s: file too small\n", filename); 
        fclose(fp); 
        return 1;
    }        

    rdi.buffer = malloc(rdi.length);
    if (!rdi.buffer) { 
        fprintf(stderr, "malloc failed\n"); 
        fclose(fp); 
        return 1; 
    } 

    // load the file to memory
    if (fread(rdi.buffer, 1, rdi.length, fp) != rdi.length) {
        fprintf(stderr, "read error\n"); 
        free(rdi.buffer); 
        fclose(fp); 
        return 1;
    }        
    fclose(fp);

    // check the MAGIC word
    if (memcmp(rdi.buffer, REGF_MAGIC, 4) != 0) {
        fprintf(stderr, "%s: not a registry hive\n", filename); 
        free(rdi.buffer); 
        return 1;
    }

    // decode the file
    int rtn_code = decode_regf_file(&rdi);

    // free memory and flush output
    free(rdi.buffer);

    fflush(stdout);
    fflush(stderr);

    return rtn_code;
}

