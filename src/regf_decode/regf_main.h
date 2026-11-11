/* regf_main.h
 *
 *
 */

#if !defined( REGF_MAIN_H )
#define REGF_MAIN_H

#include <stdint.h>   // uint8_t / uint32_t など（Linux gcc は暗黙includeされない）
#include <stddef.h>   // size_t

// the struct to hold all needed parameters for the decode program
typedef struct _REGF_DECODE_INFO {
    const char *filename;	// reg filename
    uint8_t    *buffer;		// the memory to hold whole file
    const char *filter_path;	// filter to output
    const char *dump_pattern;	// path to dump
    size_t      length;
    uint32_t    output_mode;
} REGF_DECODE_INFO;




/*
 * output_mode (uint32_t)
 *
 *  [31 .............. 16][15 .............. 0]
 *        Reserved             Output flags
 *
 *  - Low 16 bits  : output / behavior flags
 *  - High 16 bits : Reserved
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
#define OUT_VERBOSE       0x0100
#define OUT_DEBUG         OUT_VERBOSE
#define OUT_FULL_BINARY   0x0200
#define OUT_DUMP_PATTERN  0x0400
#define OUT_FILTER_PATH   0x0800

/* ============================================================
 * Masks
 * ============================================================
 */
#define OUTMODE_MASK    0x0000FFFF
#define HIGHWORD_MASK   0xFFFF0000

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





/* ─────────────────────────────────────────────
   定数
   ───────────────────────────────────────────── */
#define REGF_MAGIC       "regf"
#define HBIN_MAGIC       "hbin"
#define NK_MAGIC         "nk"
#define VK_MAGIC         "vk"
#define LF_MAGIC         "lf"
#define LH_MAGIC         "lh"
#define RI_MAGIC         "ri"
#define LI_MAGIC         "li"

#define REGF_HEADER_SIZE 0x1000
#define CELL_BASE        0x1000
#define MAX_PATH_LEN     8192       /* UTF-8 展開後はバイト数が増えるので大きめに */
#define MAX_DEPTH        512

/* NK cell flags */
#define NK_FLAG_ROOT     0x0004
#define NK_FLAG_ASCII    0x0020     /* セットされていれば ASCII, なければ UTF-16LE */

/* REG value types */
#define REG_NONE         0
#define REG_SZ           1
#define REG_EXPAND_SZ    2
#define REG_BINARY       3
#define REG_DWORD        4
#define REG_DWORD_BE     5
#define REG_LINK         6
#define REG_MULTI_SZ     7
#define REG_QWORD        11

/* vk cell flags */
#define VK_FLAG_ASCII    0x0001     /* 値名が ASCII */

/* データが vk cell 内にインライン格納される閾値 */
#define VK_INLINE_FLAG   0x80000000

/* ─────────────────────────────────────────────
   構造体
   ───────────────────────────────────────────── */
#pragma pack(push, 1)

// REGF HEADER, 4096 bytes. After this header, 1st HBIN, 2nd HBIN, etc
typedef struct {
    char        magic[4];		// offset: 0
    uint32_t    primary_seqnum;		// 4
    uint32_t    secondary_seqnum;	// 8
    uint64_t    last_written;		// 12
    uint32_t    major_version;		// 20
    uint32_t    minor_version;		// 24
    uint32_t    file_type;		// 28
    uint32_t    file_format;		// 32
    uint32_t    root_cell_offset;	// 36
    uint32_t    hive_bins_data_size;	// 40	hive bins data size, total size of all HBINs
    uint32_t    clustering_factor;	// 44
    char        filename[64];		// 48
    uint8_t     reserved[396];		// 112  (NB: allocated in Win10)
    uint32_t    checksum;		// 508: checksum from byte 0 to byte 507, i.e. 508 bytes
    uint8_t     reserved2[3576];	// 512  (NB: allocated in Win10)
    uint32_t    boot_type;		// 4088
    uint32_t    boot_recover;		// 4092
} regf_header_t;


// HIVEBIN HEADER, 32 bytes, after this header, HBIN CELLs continues
// https://github.com/msuhanov/regf/blob/master/Windows%20registry%20file%20format%20specification.md#hive-bin
typedef struct {
    char        magic[4];		// offset: 0  value=hbin
    uint32_t    hbin_offset;		// 4 Offset relative from the start of the hive bins data (CELL_BASE=0x1000)
    uint32_t    hbin_size;		// 8 size of this HBIN
    uint64_t    reserved1;		// 12
    uint64_t    timestamp;		// 20  FILETIME defined in 1st HBIN only
    uint32_t    spare;			// 28  or called as MemAlloc
} hbin_header_t;



typedef struct {
    int32_t     cell_size;
    char        magic[2];           /* "nk" or others */
} cell_header_t;





typedef struct {
    int32_t     cell_size;
    char        magic[2];           // "nk"
    uint16_t    flags;
    uint64_t    last_written;
    uint32_t    access_bits;        // Often labeled "unknown1"
    uint32_t    parent_offset;
    uint32_t    subkey_count;
    uint32_t    volatile_subkey_count;
    uint32_t    subkey_list_offset;
    uint32_t    volatile_subkey_list_offset;
    uint32_t    value_count;
    uint32_t    value_list_offset;
    uint32_t    security_offset;
    uint32_t    classname_offset;
    uint32_t    max_subkey_name_len;
    uint32_t    max_subkey_classname_len;
    uint32_t    max_value_name_len;
    uint32_t    max_value_data_len;
    uint32_t    work_var;           // Often labeled "unknown2"
    uint16_t    name_length;
    uint16_t    classname_length;
    char        name[1];            // Start of the key name
} nk_cell_t;

typedef struct {
    int32_t     cell_size;
    char        magic[2];           /* "vk" */
    uint16_t    name_length;
    uint32_t    data_size;          /* VK_INLINE_FLAG がセットされていたらインライン */
    uint32_t    data_offset;        /* インライン時はデータそのもの (4bytes) */
    uint32_t    data_type;
    uint16_t    flags;              /* VK_FLAG_ASCII */
    uint16_t    unknown;
    char        name[1];
} vk_cell_t;

typedef struct {
    uint32_t    nk_offset;
    char        hash[4];
} lf_entry_t;

typedef struct {
    int32_t     cell_size;
    char        magic[2];
    uint16_t    count;
    lf_entry_t  entries[1];
} lf_cell_t;


typedef struct {
    uint32_t    nk_offset;
    char        hash[4];
} lh_entry_t;

typedef struct {
    int32_t     cell_size;
    char        magic[2];
    uint16_t    count;
    lh_entry_t  entries[1];
} lh_cell_t;





typedef struct {
    int32_t     cell_size;
    char        magic[2];
    uint16_t    count;
    uint32_t    offsets[1];
} ri_cell_t;

/* li: ri と同じ並び。ただし offsets[] は nk を直接指す（ri は lf/lh/li を指す）。 */
typedef struct {
    int32_t     cell_size;
    char        magic[2];
    uint16_t    count;
    uint32_t    offsets[1];
} li_cell_t;

#pragma pack(pop)

























#endif /* !defined( REGF_MAIN_H ) */
