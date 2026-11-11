#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "evtx_output.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] evtxfile\n"
        "\n"
        "Output options (can be combined):\n"
        "  -c, --csv        CSV output\n"
        "  -t, --txt        Text output\n"
        "  -x, --xml        XML output\n"
        "  -s, --schema     Schema output\n"
        "  -d, --debug      Debug output\n"
        "\n"
        "Filter options:\n"
        "  -e <EventID>     Filter by EventID (e.g. 4624)\n"
        "\n"
        "If no output option is specified, DEFAULT summary output is used.\n",
        prog
    );
}


const char *check_cmd_argv(uint32_t *mode_ptr, int argc, char *argv[])
{
    uint32_t output_mode = 0;
    const char *filename = NULL;

    for (int i = 1; i < argc; i++) {

        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--csv")) {
            SET_OUTMODE(output_mode, OUT_CSV);
        }
        else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--txt")) {
            SET_OUTMODE(output_mode, OUT_TXT);
        }
        else if (!strcmp(argv[i], "-x") || !strcmp(argv[i], "--xml")) {
            SET_OUTMODE(output_mode, OUT_XML);
        }
        else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--schema")) {
            SET_OUTMODE(output_mode, OUT_SCHEMA);
        }
        else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")) {
            SET_OUTMODE(output_mode, OUT_DEBUG);
        }
        else if (!strcmp(argv[i], "-e")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ERROR: -e requires an EventID\n");
                usage(argv[0]);
                return NULL;
            }
            uint32_t evtid = (uint32_t)atoi(argv[++i]);
            SET_EVTID(output_mode, evtid);
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return NULL;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return NULL;
        }
        else {
            /* positional argument = filename */
            filename = argv[i];
        }
    }

    /* set output_mode AFTER parsing all args */
    *mode_ptr = output_mode;

    return filename;
}



int main(int argc, char *argv[])
{
    uint32_t output_mode = 0;
    const char *filename = check_cmd_argv(&output_mode, argc, argv);

    if (!filename) {
        fprintf(stderr, "ERROR: no evtx file specified\n");
        usage(argv[0]);
        return 1;
    }

    /* ---- Debug print of parsed state ---- */

    printf("Input file : %s\n", filename);
    printf("EventID    : ");
    if (GET_EVTID(output_mode))
        printf("%u\n", GET_EVTID(output_mode));
    else
        printf("(none)\n");

    printf("Output mode:\n");

    if (IS_OUT_DEFAULT(output_mode))
        printf("  DEFAULT summary output\n");

    if (CHECK_OUTMODE(output_mode, OUT_CSV))
        printf("  CSV\n");
    if (CHECK_OUTMODE(output_mode, OUT_TXT))
        printf("  TXT\n");
    if (CHECK_OUTMODE(output_mode, OUT_XML))
        printf("  XML\n");
    if (CHECK_OUTMODE(output_mode, OUT_SCHEMA))
        printf("  SCHEMA\n");
    if (CHECK_OUTMODE(output_mode, OUT_DEBUG))
        printf("  DEBUG\n");

    /*
     * Here is where real code would go:
     *
     *   decode_evtx_file(filename, output_mode);
     */

    return 0;
}
