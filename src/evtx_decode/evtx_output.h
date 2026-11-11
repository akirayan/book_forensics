#ifndef EVTX_OUTPUT_H
#define EVTX_OUTPUT_H

#include <stdint.h>
#include "evtx_xmltree.h"


/*
 * output_mode (uint32_t)
 *
 *  [31 .............. 16][15 .............. 0]
 *        EventID               Output flags
 *
 *  - Low 16 bits  : output / behavior flags
 *  - High 16 bits : EventID filter (0 = no filter)
 */

/* ============================================================
 * Output format flags (low 16 bits)
 * ============================================================
 * These define WHAT to output.
 * If none of these flags are set, output is DEFAULT summary.
 */
#define OUT_NONE        0x0000
#define OUT_CSV         0x0001
#define OUT_TXT         0x0002
#define OUT_XML         0x0004
#define OUT_RESVER5     0x0008
#define OUT_RESVER4     0x0010
#define OUT_RESVER3     0x0020
#define OUT_RESVER2     0x0040
#define OUT_RESVER1     0x0080

/* ============================================================
 * Auxiliary / behavior flags (low 16 bits)
 * ============================================================
 * These modify behavior and can coexist with any format.
 */
#define OUT_DEBUG       0x0100

/* ============================================================
 * Masks
 * ============================================================
 */
#define OUTMODE_MASK    0x0000FFFF
#define EVTID_MASK      0xFFFF0000

/* Mask for format-related flags only */
#define OUTFMT_MASK     (OUT_CSV | OUT_TXT | OUT_XML)

/* ============================================================
 * Output mode helpers
 * ============================================================
 */

/* Set one or more output flags */
#define SET_OUTMODE(mode, flag) \
    ((mode) |= ((flag) & OUTMODE_MASK))

/* Clear one or more output flags */
#define CLEAR_OUTMODE(mode, flag) \
    ((mode) &= ~((flag) & OUTMODE_MASK))

/* Check whether a flag is set */
#define CHECK_OUTMODE(mode, flag) \
    (((mode) & (flag)) != 0)

/* ============================================================
 * DEFAULT mode detection
 * ============================================================
 * DEFAULT means:
 *   - no CSV / TXT / XML / SCHEMA / VERBOSE explicitly requested
 *   - DEBUG may be ON or OFF
 */
#define IS_OUT_DEFAULT(mode) \
    (((mode) & OUTFMT_MASK) == 0)

/* Any explicit output format requested? */
#define HAS_OUTFMT(mode) \
    (((mode) & OUTFMT_MASK) != 0)

/* Clear all format flags but keep DEBUG etc. */
#define CLEAR_OUTFMT(mode) \
    ((mode) &= ~OUTFMT_MASK)

/* ============================================================
 * EventID helpers (high 16 bits)
 * ============================================================
 */

/* Set EventID filter */
#define SET_EVTID(mode, id) \
    ((mode) = ((mode) & OUTMODE_MASK) | ((uint32_t)(id) << 16))

/* Get EventID filter (0 = no filter) */
#define GET_EVTID(mode) \
    (((mode) & EVTID_MASK) >> 16)

/* Clear EventID filter */
#define CLEAR_EVTID(mode) \
    ((mode) &= OUTMODE_MASK)




void output_xmltree(XML_TREE *xtree, uint32_t output_mode);






#undef TEST_CODE


#ifdef TEST_CODE

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


#endif









#endif /* EVTX_OUTPUT_H */
