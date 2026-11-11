/*
 * gpt_decode.c - Disk Partition Table Decoder (MBR + GPT)
 *
 * Reads a raw disk image or block device and decodes:
 *   LBA 0       : MBR (Protective or legacy)
 *                   - 4 primary partitions
 *                   - Extended partition (EBR chain) if present
 *   LBA 1       : GPT Header (Primary)         [only if GPT disk]
 *   LBA 2+      : GPT Partition Entry Array    [only if GPT disk]
 *   Backup LBA  : GPT Header (Backup)          [only if GPT disk]
 *   Backup-32   : GPT Partition Entry Array    [only if GPT disk]
 *
 * Architecture (layered):
 *   Layer 1 - Binary structures  : on-disk packed structs
 *   Layer 2 - Parsed structures  : interpreted/decoded data
 *   Layer 3 - Read functions     : file I/O -> parsed structs
 *   Layer 4 - Print functions    : parsed structs -> text output
 *   Layer 5 - main               : orchestration only
 *
 * Usage:
 *   ./gpt_decode <device_or_image> [device_number]
 *   ./gpt_decode /dev/sda
 *   ./gpt_decode /dev/loop0 2
 *   ./gpt_decode hdd.img
 *
 * Build:
 *   gcc -Wall -Wextra -o gpt_decode gpt_decode.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  LAYER 1 - Binary On-Disk Structures                               */
/* ================================================================== */

#define SECTOR_SIZE              512
#define MBR_SIGNATURE_VALUE      0xAA55
#define GPT_SIGNATURE_VALUE      0x5452415020494645ULL  /* "EFI PART" LE */
#define GPT_HEADER_SIZE          92
#define GPT_MAX_ENTRIES          128
#define GPT_ENTRY_SIZE           128
#define EBR_MAX_LOGICAL          64   /* safety limit for EBR chain walk */

/* MBR Partition Entry (16 bytes) */
typedef struct {
    uint8_t  boot_indicator;
    uint8_t  starting_head;
    uint8_t  starting_sector_low    : 6;
    uint8_t  starting_cylinder_high : 2;
    uint8_t  starting_cylinder;
    uint8_t  system_id;
    uint8_t  ending_head;
    uint8_t  ending_sector_low      : 6;
    uint8_t  ending_cylinder_high   : 2;
    uint8_t  ending_cylinder;
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) BinMBR_Entry;

/* MBR (512 bytes) */
typedef struct {
    uint8_t      boot_code[440];
    uint8_t      disk_signature[4];
    uint16_t     reserved;
    BinMBR_Entry partitions[4];
    uint16_t     signature;
} __attribute__((packed)) BinMBR;

/* EBR sector - same layout as MBR, only first 2 entries used */
typedef struct {
    uint8_t      ignored[446];
    BinMBR_Entry entries[4];
    uint16_t     signature;
} __attribute__((packed)) BinEBR;

/* GPT Header (512 bytes) */
typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved1;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
    uint8_t  reserved2[420];
} __attribute__((packed)) BinGPT_Header;

/* GPT Partition Entry (128 bytes) */
typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attributes;
    uint16_t name[36];   /* UTF-16LE */
} __attribute__((packed)) BinGPT_Entry;

/* ================================================================== */
/*  LAYER 2 - Parsed / Interpreted Structures                         */
/* ================================================================== */

/* One parsed MBR primary partition */
typedef struct {
    int      used;
    int      slot;            /* 1-4 */
    uint8_t  boot_indicator;
    uint8_t  system_id;
    uint32_t lba_start;
    uint32_t sector_count;
} ParsedMBR_Part;

/* One parsed logical partition (from EBR chain) */
typedef struct {
    int      slot;            /* 5, 6, 7, ... */
    uint32_t ebr_lba;         /* LBA of the EBR that describes this partition */
    uint32_t lba_start;       /* absolute LBA of the logical partition */
    uint32_t sector_count;
    uint8_t  system_id;
} ParsedLogical;

/* Complete parsed MBR information */
typedef struct {
    int            valid;            /* 1 = signature 0xAA55 OK */
    uint8_t        disk_sig[4];
    ParsedMBR_Part primary[4];
    int            ext_slot;         /* which primary slot is Extended (-1 if none) */
    uint32_t       ext_lba_start;    /* absolute LBA of Extended partition */
    ParsedLogical  logical[EBR_MAX_LOGICAL];
    int            logical_count;
    int            is_protective;    /* 1 = first entry is type 0xEE */
} ParsedMBR;

/* One parsed GPT partition entry */
typedef struct {
    int      valid;
    int      slot;            /* 1-based index in entry array */
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attributes;
    char     name_utf8[128];
} ParsedGPT_Entry;

/* Parsed GPT header + entries (used for both primary and backup) */
typedef struct {
    int             valid;           /* 1 = signature OK */
    BinGPT_Header   hdr;
    ParsedGPT_Entry entries[GPT_MAX_ENTRIES];
    int             entry_count;     /* number of non-empty entries */
    char            disk_guid_str[37];
    int             entries_read;    /* 1 = entry array was successfully read */
} ParsedGPT;

/* ================================================================== */
/*  Lookup Tables                                                      */
/* ================================================================== */

typedef struct { uint8_t id; const char *desc; } MBR_TypeEntry;
static const MBR_TypeEntry mbr_types[] = {
    {0x00, "Empty"},
    {0x01, "FAT12"},
    {0x04, "FAT16 (<32MB)"},
    {0x05, "Extended (CHS)"},
    {0x06, "FAT16"},
    {0x07, "NTFS / exFAT / HPFS"},
    {0x0B, "FAT32"},
    {0x0C, "FAT32 (LBA)"},
    {0x0F, "Extended (LBA)"},
    {0x11, "Hidden FAT12"},
    {0x14, "Hidden FAT16 (<32MB)"},
    {0x16, "Hidden FAT16"},
    {0x17, "Hidden NTFS / exFAT"},
    {0x1B, "Hidden FAT32"},
    {0x1C, "Hidden FAT32 (LBA)"},
    {0x27, "Windows RE (hidden)"},
    {0x42, "Windows Dynamic / LDM"},
    {0x82, "Linux Swap / Solaris"},
    {0x83, "Linux Filesystem"},
    {0x84, "OS/2 Boot Manager"},
    {0x85, "Linux Extended"},
    {0x86, "Linux RAID"},
    {0x8E, "Linux LVM"},
    {0xA5, "FreeBSD"},
    {0xA6, "OpenBSD"},
    {0xA8, "macOS X"},
    {0xAF, "macOS X HFS+"},
    {0xEE, "GPT Protective MBR"},
    {0xEF, "EFI System Partition (FAT)"},
    {0xFB, "VMware VMFS"},
    {0xFD, "Linux RAID auto-detect"},
    {0xFE, "LANstep / IBM IML"},
};
static const char* mbr_type_str(uint8_t id) {
    size_t n = sizeof(mbr_types) / sizeof(mbr_types[0]);
    for (size_t i = 0; i < n; i++)
        if (mbr_types[i].id == id) return mbr_types[i].desc;
    return "Unknown";
}

typedef struct { const uint8_t bytes[16]; const char *desc; } GPT_TypeEntry;
static const GPT_TypeEntry gpt_types[] = {
    {{0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B},
     "EFI System Partition (ESP)"},
    {{0x16,0xE3,0xC9,0xE3,0x5C,0x0B,0xB8,0x4D,0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE},
     "Microsoft Reserved Partition (MSR)"},
    {{0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7},
     "Microsoft Basic Data Partition"},
    {{0xA4,0xBB,0x94,0xDE,0xD1,0x06,0x40,0x4D,0xA1,0x6A,0xBF,0xD5,0x01,0x79,0xD6,0xAC},
     "Windows Recovery Environment"},
    {{0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4},
     "Linux Filesystem"},
    {{0x6D,0xFD,0x57,0x06,0xE8,0xA4,0x33,0x4B,0xA2,0x3F,0xB8,0xF4,0x33,0x60,0xD7,0x07},
     "Linux Swap"},
    {{0x79,0xD3,0xD6,0xE6,0x07,0xF5,0xC2,0x44,0xA2,0x3C,0x23,0x8F,0x2A,0x3A,0xD0,0x00},
     "Linux LVM"},
    {{0x00,0x53,0x46,0x48,0x00,0x00,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC},
     "Apple HFS+"},
    {{0xEF,0x57,0x34,0x7C,0x00,0x00,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC},
     "Apple APFS"},
    {{0x5D,0x2A,0x3A,0xFE,0x32,0x4F,0xA7,0x41,0xB7,0x25,0xAC,0xCC,0x32,0x85,0xA3,0x09},
     "ChromeOS Kernel"},
};
static const char* gpt_type_str(const uint8_t *guid) {
    size_t n = sizeof(gpt_types) / sizeof(gpt_types[0]);
    for (size_t i = 0; i < n; i++)
        if (memcmp(gpt_types[i].bytes, guid, 16) == 0) return gpt_types[i].desc;
    return "Unknown Type";
}

/* ================================================================== */
/*  LAYER 2 - Utility Helpers (no I/O)                               */
/* ================================================================== */

/* Format 16-byte on-disk GUID to lowercase canonical string.
 * On-disk mixed-endian: [0..3] DWORD LE, [4..5] WORD LE, [6..7] WORD LE, [8..15] as-is */
static void format_guid(char *buf, const uint8_t *g) {
    snprintf(buf, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g[3],g[2],g[1],g[0], g[5],g[4], g[7],g[6],
        g[8],g[9], g[10],g[11],g[12],g[13],g[14],g[15]);
}

/* UTF-16LE (max max_chars code units) -> UTF-8 */
static void utf16le_to_utf8(char *out, size_t out_size,
                              const uint16_t *in, size_t max_chars) {
    size_t pos = 0;
    for (size_t i = 0; i < max_chars && in[i] != 0; i++) {
        uint16_t c = in[i];
        if (c < 0x0080) {
            if (pos + 1 >= out_size) break;
            out[pos++] = (char)(c & 0x7F);
        } else if (c < 0x0800) {
            if (pos + 2 >= out_size) break;
            out[pos++] = (char)(0xC0 | (c >> 6));
            out[pos++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (pos + 3 >= out_size) break;
            out[pos++] = (char)(0xE0 | (c >> 12));
            out[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[pos] = '\0';
}

/* Sector count -> human-readable size string */
static void format_size(char *buf, size_t buf_size, uint64_t sectors) {
    double bytes = (double)sectors * SECTOR_SIZE;
    if      (bytes >= 1024.0*1024.0*1024.0)
        snprintf(buf, buf_size, "%.2f GB", bytes / (1024.0*1024.0*1024.0));
    else if (bytes >= 1024.0*1024.0)
        snprintf(buf, buf_size, "%.2f MB", bytes / (1024.0*1024.0));
    else
        snprintf(buf, buf_size, "%.2f KB", bytes / 1024.0);
}

/* ================================================================== */
/*  LAYER 3 - Read Functions (file I/O -> parsed structs)             */
/* ================================================================== */

static int seek_lba(FILE *f, uint64_t lba) {
    if (fseeko(f, (off_t)(lba * SECTOR_SIZE), SEEK_SET) != 0) {
        perror("fseeko"); return -1;
    }
    return 0;
}

/* read_mbr: reads LBA 0, populates ParsedMBR (without EBR walk) */
static int read_mbr(FILE *f, ParsedMBR *out) {
    memset(out, 0, sizeof(*out));
    out->ext_slot = -1;

    BinMBR raw;
    if (seek_lba(f, 0) != 0) return -1;
    if (fread(&raw, 1, sizeof(raw), f) != sizeof(raw)) {
        fprintf(stderr, "Error: Failed to read LBA 0 (MBR).\n"); return -1;
    }

    out->valid = (raw.signature == MBR_SIGNATURE_VALUE) ? 1 : 0;
    memcpy(out->disk_sig, raw.disk_signature, 4);

    for (int i = 0; i < 4; i++) {
        BinMBR_Entry  *e = &raw.partitions[i];
        ParsedMBR_Part *p = &out->primary[i];
        p->slot           = i + 1;
        p->used           = (e->system_id != 0x00 || e->sector_count != 0) ? 1 : 0;
        p->boot_indicator = e->boot_indicator;
        p->system_id      = e->system_id;
        p->lba_start      = e->lba_start;
        p->sector_count   = e->sector_count;

        if (e->system_id == 0x05 || e->system_id == 0x0F || e->system_id == 0x85) {
            out->ext_slot      = i;
            out->ext_lba_start = e->lba_start;
        }
    }
    if (out->primary[0].system_id == 0xEE)
        out->is_protective = 1;

    return 0;
}

/* walk_extended: follows EBR chain, appends to mbr->logical[] */
static void walk_extended(FILE *f, ParsedMBR *mbr) {
    if (mbr->ext_slot < 0) return;

    uint32_t ext_base = mbr->ext_lba_start;
    uint32_t ebr_lba  = ext_base;
    int      slot_n   = 5;

    for (int guard = 0; guard < EBR_MAX_LOGICAL; guard++) {
        BinEBR ebr;
        if (seek_lba(f, (uint64_t)ebr_lba) != 0) break;
        if (fread(&ebr, 1, sizeof(ebr), f) != sizeof(ebr)) break;
        if (ebr.signature != MBR_SIGNATURE_VALUE) {
            fprintf(stderr, "Warning: Invalid EBR signature at LBA %u (0x%04X).\n",
                    ebr_lba, ebr.signature);
            break;
        }

        /* Entry[0]: logical partition, LBA relative to this EBR */
        BinMBR_Entry *e0 = &ebr.entries[0];
        if (e0->sector_count == 0) break;

        if (mbr->logical_count >= EBR_MAX_LOGICAL) break;
        ParsedLogical *lp = &mbr->logical[mbr->logical_count++];
        lp->slot         = slot_n++;
        lp->ebr_lba      = ebr_lba;
        lp->lba_start    = ebr_lba + e0->lba_start;   /* absolute LBA */
        lp->sector_count = e0->sector_count;
        lp->system_id    = e0->system_id;

        /* Entry[1]: next EBR, LBA relative to ext_base; 0 = no more */
        BinMBR_Entry *e1 = &ebr.entries[1];
        if (e1->sector_count == 0) break;
        ebr_lba = ext_base + e1->lba_start;
    }
}

/* read_gpt_header: reads one 512-byte sector, verifies signature
 * Returns: 0=OK, -1=I/O error, +1=signature mismatch (not GPT) */
static int read_gpt_header(FILE *f, uint64_t lba, BinGPT_Header *out) {
    if (seek_lba(f, lba) != 0) return -1;
    if (fread(out, 1, sizeof(*out), f) != sizeof(*out)) {
        fprintf(stderr, "Error: Failed to read GPT header at LBA %llu.\n",
                (unsigned long long)lba);
        return -1;
    }
    return (out->signature != GPT_SIGNATURE_VALUE) ? 1 : 0;
}

/* parse_gpt: reads header + entry array, fills ParsedGPT */
static int parse_gpt(FILE *f, uint64_t header_lba, ParsedGPT *out) {
    memset(out, 0, sizeof(*out));

    int rc = read_gpt_header(f, header_lba, &out->hdr);
    if (rc != 0) return -1;
    out->valid = 1;
    format_guid(out->disk_guid_str, out->hdr.disk_guid);

    uint32_t max_e = out->hdr.num_partition_entries;
    if (max_e == 0 || max_e > GPT_MAX_ENTRIES) max_e = GPT_MAX_ENTRIES;

    BinGPT_Entry raw[GPT_MAX_ENTRIES];
    memset(raw, 0, sizeof(raw));

    if (seek_lba(f, out->hdr.partition_entry_lba) != 0) return 0;
    uint32_t n = (uint32_t)fread(raw, sizeof(BinGPT_Entry), max_e, f);
    out->entries_read = (n > 0) ? 1 : 0;

    int pnum = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t *g = (const uint64_t *)raw[i].type_guid;
        if (g[0] == 0 && g[1] == 0) continue;

        ParsedGPT_Entry *pe = &out->entries[pnum++];
        pe->valid = 1;
        pe->slot  = (int)(i + 1);
        memcpy(pe->type_guid,   raw[i].type_guid,   16);
        memcpy(pe->unique_guid, raw[i].unique_guid, 16);
        pe->start_lba  = raw[i].start_lba;
        pe->end_lba    = raw[i].end_lba;
        pe->attributes = raw[i].attributes;

        /* aligned copy avoids packed-member warning */
        uint16_t name_copy[36];
        memcpy(name_copy, raw[i].name, sizeof(name_copy));
        utf16le_to_utf8(pe->name_utf8, sizeof(pe->name_utf8), name_copy, 36);
    }
    out->entry_count = pnum;
    return 0;
}

/* ================================================================== */
/*  LAYER 4 - Print Functions (parsed structs -> text output)         */
/* ================================================================== */

static void print_mbr_table(const ParsedMBR *mbr) {
    printf("\n--- LBA 0: MBR (Master Boot Record) ---\n");
    printf("  Disk Signature: %02X %02X %02X %02X\n",
           mbr->disk_sig[0], mbr->disk_sig[1],
           mbr->disk_sig[2], mbr->disk_sig[3]);
    printf("  MBR Signature:  0x%04X (%s)\n",
           MBR_SIGNATURE_VALUE, mbr->valid ? "OK" : "INVALID");
    printf("  Type:           %s\n",
           mbr->is_protective ? "Protective MBR (GPT disk)" : "Legacy MBR");

    printf("\n  Primary Partition Table:\n");
    printf("  %-6s | %-4s | %10s | %10s | %10s | %2s | %-26s\n",
           "Device","Boot","Start LBA","Sectors","Size","Id","Type");
    printf("  -------+------+------------+------------+------------"
           "+----+----------------------------\n");

    for (int i = 0; i < 4; i++) {
        const ParsedMBR_Part *p = &mbr->primary[i];
        char size_buf[24] = "-";
        if (p->used && p->sector_count > 0)
            format_size(size_buf, sizeof(size_buf), p->sector_count);

        printf("  P%-5d | %-4s | %10u | %10u | %10s | %02X | %-26s\n",
               p->slot,
               (p->used && p->boot_indicator == 0x80) ? "*" : " ",
               p->used ? p->lba_start    : 0,
               p->used ? p->sector_count : 0,
               size_buf,
               p->system_id,
               mbr_type_str(p->system_id));
    }

    if (mbr->logical_count > 0) {
        printf("\n  Logical Partitions (Extended / EBR chain):\n");
        printf("  %-6s | %10s | %10s | %10s | %2s | %-26s | %s\n",
               "Device","Start LBA","Sectors","Size","Id","Type","EBR LBA");
        printf("  -------+------------+------------+------------"
               "+----+----------------------------+----------\n");
        for (int i = 0; i < mbr->logical_count; i++) {
            const ParsedLogical *lp = &mbr->logical[i];
            char size_buf[24];
            format_size(size_buf, sizeof(size_buf), lp->sector_count);
            printf("  L%-5d | %10u | %10u | %10s | %02X | %-26s | %u\n",
                   lp->slot, lp->lba_start, lp->sector_count, size_buf,
                   lp->system_id, mbr_type_str(lp->system_id), lp->ebr_lba);
        }
    }
    printf("  ------------------------------------------------------------------"
           "-------------------\n");
}

static void print_gpt_header(const ParsedGPT *gpt, const char *label) {
    const BinGPT_Header *h = &gpt->hdr;
    printf("\n--- GPT Header (%s, LBA %llu) ---\n",
           label, (unsigned long long)h->current_lba);
    printf("  Revision:           v%u.%u\n",
           (h->revision >> 16) & 0xFFFF, h->revision & 0xFFFF);
    printf("  Header Size:        %u bytes (spec: %u)\n",
           h->header_size, GPT_HEADER_SIZE);
    printf("  Header CRC32:       0x%08X\n", h->header_crc32);
    printf("  Disk GUID:          %s\n", gpt->disk_guid_str);
    printf("  Current LBA:        %llu\n", (unsigned long long)h->current_lba);
    printf("  Backup LBA:         %llu\n", (unsigned long long)h->backup_lba);
    printf("  First Usable LBA:   %llu\n", (unsigned long long)h->first_usable_lba);
    printf("  Last Usable LBA:    %llu\n", (unsigned long long)h->last_usable_lba);

    uint64_t usable = h->last_usable_lba - h->first_usable_lba + 1;
    char size_buf[24];
    format_size(size_buf, sizeof(size_buf), usable);
    printf("  Usable Size:        %s  (%llu sectors)\n",
           size_buf, (unsigned long long)usable);

    printf("  Entry Array LBA:    %llu\n", (unsigned long long)h->partition_entry_lba);
    printf("  Max Entries:        %u\n",   h->num_partition_entries);
    printf("  Entry Size:         %u bytes\n", h->partition_entry_size);
    printf("  Entry Array CRC32:  0x%08X\n", h->partition_array_crc32);
}

static void print_gpt_entries(const ParsedGPT *gpt, const char *label) {
    printf("\n--- GPT Partition Entries (%s) ---\n", label);
    printf("  Partition Count: %d\n", gpt->entry_count);
    printf("  -----------------------------------------\n");

    for (int i = 0; i < gpt->entry_count; i++) {
        const ParsedGPT_Entry *e = &gpt->entries[i];
        char type_str[37], uniq_str[37];
        format_guid(type_str, e->type_guid);
        format_guid(uniq_str, e->unique_guid);

        uint64_t sector_count = e->end_lba - e->start_lba + 1;
        uint64_t start_offset = e->start_lba * SECTOR_SIZE;
        uint64_t size_bytes   = sector_count * SECTOR_SIZE;
        char size_buf[24];
        format_size(size_buf, sizeof(size_buf), sector_count);

        printf("  [%d] Partition Number: %d\n", i + 1, e->slot);
        printf("    Starting Offset: 0x%016llX (LBA: %llu)\n",
               (unsigned long long)start_offset,
               (unsigned long long)e->start_lba);
        printf("    Partition Size:  0x%016llX (%s)\n",
               (unsigned long long)size_bytes, size_buf);
        printf("    Attributes:      0x%08llX\n",
               (unsigned long long)(e->attributes & 0xFFFFFFFF));
        printf("    Type GUID:       %s (%s)\n",
               type_str, gpt_type_str(e->type_guid));
        printf("    Unique GUID:     %s\n", uniq_str);
        printf("    Name:            %s\n", e->name_utf8);
    }
    printf("  -----------------------------------------\n");
}

static void print_gpt_backup_status(const ParsedGPT *primary,
                                     const ParsedGPT *backup) {
    printf("\n--- Backup GPT Consistency Check ---\n");

    if (!backup->valid) {
        printf("  [WARNING] Backup GPT header is invalid or unreadable!\n");
        return;
    }

    int ok = 1;

    if (memcmp(primary->hdr.disk_guid, backup->hdr.disk_guid, 16) != 0) {
        printf("  [WARNING] Disk GUID mismatch!\n");
        printf("    Primary: %s\n", primary->disk_guid_str);
        printf("    Backup:  %s\n", backup->disk_guid_str);
        ok = 0;
    }
    if (primary->hdr.partition_array_crc32 != backup->hdr.partition_array_crc32) {
        printf("  [WARNING] Entry array CRC32 mismatch!\n");
        printf("    Primary: 0x%08X\n", primary->hdr.partition_array_crc32);
        printf("    Backup:  0x%08X\n", backup->hdr.partition_array_crc32);
        ok = 0;
    }
    if (primary->hdr.backup_lba != backup->hdr.current_lba) {
        printf("  [WARNING] Primary->backup_lba (%llu) != Backup->current_lba (%llu)\n",
               (unsigned long long)primary->hdr.backup_lba,
               (unsigned long long)backup->hdr.current_lba);
        ok = 0;
    }
    if (backup->hdr.backup_lba != primary->hdr.current_lba) {
        printf("  [WARNING] Backup->backup_lba (%llu) != Primary->current_lba (%llu)\n",
               (unsigned long long)backup->hdr.backup_lba,
               (unsigned long long)primary->hdr.current_lba);
        ok = 0;
    }
    if (primary->entry_count != backup->entry_count) {
        printf("  [NOTE] Entry count: Primary=%d, Backup=%d\n",
               primary->entry_count, backup->entry_count);
    }

    if (ok)
        printf("  [OK] Backup GPT matches primary "
               "(Disk GUID, Entry CRC32, LBA cross-ref).\n");
}

/* ================================================================== */
/*  LAYER 5 - main (orchestration only)                               */
/* ================================================================== */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device_or_image> [device_number]\n", argv[0]);
        fprintf(stderr, "  e.g. %s /dev/sda\n", argv[0]);
        fprintf(stderr, "  e.g. %s /dev/loop0 2\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *path  = argv[1];
    int         devno = (argc >= 3) ? atoi(argv[2]) : 1;

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return EXIT_FAILURE; }

    printf("=== Device #%d: %s ===\n", devno, path);

    /* ----------------------------------------------------------------
     * Step 1: Read LBA 0 (MBR).
     *   - Parse the four primary partition entries.
     *   - If legacy MBR: also walk the EBR chain for logical partitions.
     *   - Print the MBR partition table unconditionally.
     *   - Detect 0xEE in P1 -> GPT disk (Protective MBR).
     * ---------------------------------------------------------------- */
    ParsedMBR mbr;
    if (read_mbr(f, &mbr) != 0) { fclose(f); return EXIT_FAILURE; }

    if (!mbr.is_protective && mbr.ext_slot >= 0)
        walk_extended(f, &mbr);

    print_mbr_table(&mbr);   /* always print LBA 0 contents */

    /* ----------------------------------------------------------------
     * Step 2b: Legacy MBR disk (no 0xEE found).
     *   MBR table already printed above; nothing more to do.
     * ---------------------------------------------------------------- */
    if (!mbr.is_protective) {
        printf("\nDisk Type: Legacy MBR  (no Protective 0xEE entry)\n");
        fclose(f);
        return EXIT_SUCCESS;
    }

    /* ----------------------------------------------------------------
     * Step 2a: Protective MBR confirmed (0xEE found) -> GPT disk.
     *   Read Primary GPT header (LBA 1) + partition entry array.
     * ---------------------------------------------------------------- */
    ParsedGPT gpt_primary;
    if (parse_gpt(f, 1, &gpt_primary) != 0) {
        fprintf(stderr,
                "Error: Protective MBR (0xEE) found but no valid GPT "
                "signature at LBA 1.\n");
        fclose(f);
        return EXIT_FAILURE;
    }

    /* ----------------------------------------------------------------
     * Step 2a (cont.): Read Backup GPT header (last LBA of disk).
     * ---------------------------------------------------------------- */
    ParsedGPT gpt_backup;
    memset(&gpt_backup, 0, sizeof(gpt_backup));
    parse_gpt(f, gpt_primary.hdr.backup_lba, &gpt_backup);

    fclose(f);

    /* ---- Output: Primary GPT ---- */
    print_gpt_header(&gpt_primary, "Primary");
    print_gpt_entries(&gpt_primary, "Primary");

    /* ---- Output: Backup GPT ---- */
    if (gpt_backup.valid) {
        print_gpt_header(&gpt_backup, "Backup");
        print_gpt_entries(&gpt_backup, "Backup");
    } else {
        printf("\n--- GPT Header (Backup, LBA %llu) ---\n",
               (unsigned long long)gpt_primary.hdr.backup_lba);
        printf("  [ERROR] No valid GPT signature at backup LBA.\n");
    }

    /* ---- Output: Consistency check ---- */
    print_gpt_backup_status(&gpt_primary, &gpt_backup);

    return EXIT_SUCCESS;
}
