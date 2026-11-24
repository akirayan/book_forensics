/*
 * reg_ptcache.c
 *
 *  version 0.0  2026-02-18
 *  version 0.1  2026-02-19 handling multiple devices
 *  version 0.2  2026-02-23 struct definitions, dynamic header size
 *  version 0.3  2026-02-23 corrected entry layout from empirical analysis
 *                          entry_size=144, TypeGUID@+32, UniqueGUID@+48, Name@+72
 *                          MBR support (version=0, Disk Signature)
 *  version 0.4  2026-03-01 added CSV output mode (--csv flag)
 *  version 0.5  2026-03-04 decoded LBA info (first 32 bytes)
 *  version 0.6  2026-03-11 multi-format input support:
 *                          (1) regedit.exe hex dump  "PartitionTableCache"=hex:...
 *                          (2) reg_decode pipe       .../PartitionTableCache|REG_BINARY|...
 *                          (3) evtx_decode TXT       Event.EventData.Data.PartitionTable: ...
 *  version 0.8  2026-03-11 (2) reg_decode: extract Ven_/Prod_/Serial from path -> DeviceLabel
 *                          (3) evtx_decode TXT: extract Manufacturer/Model/SerialNumber -> DeviceLabel
 *                          (4) evtx_decode XML: <Data Name="PartitionTable">... support
 *                              XML: extract Manufacturer/Model/SerialNumber -> DeviceLabel
 *                          fixed MBR CSV column order (Boot Indicator / System ID)
 *                          fixed CSV header columns (DeviceLabel,StartOffset,Size,Attributes)
 *  version 0.9  2026-03-11 fixed FORMAT_EVTX/FORMAT_XML loop termination bug:
 *  version 1.0  2026-03-11 FORMAT_REGEDIT: extract Ven_/Prod_ from [HKEY_...] section
 *                          header line -> DeviceLabel (serial not available in this format)
 *                          Windows logs disconnect events with PartitionTable=[Empty]
 *                          (PartitionCount=0). collect_hex("[Empty]") produced len=0,
 *                          causing early return 0 and stopping the main loop.
 *                          Fixed by outer retry loop in extract_partition_table_cache():
 *                          len==0 with !feof(stdin) => [Empty] event => skip and retry.
 *  (c) Akira Endo (yan.akira@gmail.com)
 *
 *
 * Purpose:
 *   Parse "PartitionTableCache" (from SYSTEM registry) or "PartitionTable"
 *   (from Microsoft-Windows-Partition/Diagnostic Event ID 1006) and display
 *   Disk GUID and Partition GUID information.
 *
 * Usage:
 *   ./reg_ptcache [options]
 *
 *   Options:
 *     -c | --csv    CSV output mode (default: human-readable)
 *
 * Expected Input (auto-detected, any of the following):
 *
 *   (1) regedit.exe hex dump (UTF-16LE converted to UTF-8):
 *
 *     iconv -f utf16le -t utf8 < disk.reg | ./reg_ptcache
 *     iconv -f utf16le -t utf8 < disk.reg | ./reg_ptcache --csv
 *
 *   (2) reg_decode pipe-delimited output:
 *
 *     regf_decode -f SYSTEM | grep "Partmgr/PartitionTableCache|REG_BINARY" | ./reg_ptcache
 *     regf_decode -f SYSTEM | grep "Partmgr/PartitionTableCache|REG_BINARY" | ./reg_ptcache --csv
 *
 *   (3) evtx_decode TXT output:
 *
 *     evtx_decode -t -e 1006 Microsoft-Windows-Partition-Diagnostic.evtx | ./reg_ptcache
 *     evtx_decode -t -e 1006 Microsoft-Windows-Partition-Diagnostic.evtx | ./reg_ptcache --csv
 *
 * CSV Columns (with --csv):
 *   Device,DiskType,DiskID,PartNo,TypeGUID,TypeName,UniqueGUID,Name
 *
 * Structure (empirically derived from multiple devices):
 *
 *   PTCache_Header (48 bytes, fixed):
 *     offset  0: version     (uint32)   0=MBR, 1=GPT
 *     offset  4: part_count  (uint32)
 *     offset  8: disk_id     (16 bytes) GPT: Disk GUID / MBR: Disk Signature(4) + padding
 *     offset 24: unknown     (24 bytes)
 *
 *   PTCache_Entry (144 bytes each):
 *     offset   0: prefix     (32 bytes) LBA range info etc. DECODED in ver 0.5 (see struct)
 *     offset  32: type_guid  (16 bytes) GPT partition type GUID
 *     offset  48: uniq_guid  (16 bytes) GPT partition unique GUID
 *     offset  64: unknown    (8 bytes)
 *     offset  72: name       (72 bytes) UTF-16LE partition name
 *
 *   Total = 48 + part_count * 144
 *
 *   Verified against:
 *     GPT: JMicron(7), KIOXIA(2x), KINGSTON(4), Realtek(5)
 *     MBR: USB DISK 3.0(4)
 *
 * Forensic Notes:
 *   - PartitionTableCache is a Windows PartMgr cache of GPT/MBR information.
 *   - Disk GUID and Partition Unique GUID can be correlated with the physical disk.
 *   - Partition Unique GUIDs can be matched with Microsoft-Windows-Partition/Diagnostic
 *     Event ID 1006 to link disk activity to specific devices.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Layout constants (empirically verified)                            */
/* ------------------------------------------------------------------ */

#define PTCACHE_HEADER_SIZE     48   /* fixed header */
#define PTCACHE_ENTRY_SIZE     144   /* per partition entry */

#define ENTRY_OFF_PREFIX         0   /* 32 bytes: LBA info etc. */
#define ENTRY_OFF_TYPE_GUID     32   /* 16 bytes: GPT Type GUID */
#define ENTRY_OFF_UNIQ_GUID     48   /* 16 bytes: GPT Unique GUID */
#define ENTRY_OFF_UNKNOWN       64   /*  8 bytes: unknown */
#define ENTRY_OFF_NAME          72   /* 72 bytes: UTF-16LE name */

#define ENTRY_NAME_MAXBYTES     72

#define INITIAL_BUF_SIZE       256

/* ------------------------------------------------------------------ */
/* Global options                                                      */
/* ------------------------------------------------------------------ */

static int g_csv_mode = 0;   /* 1 = CSV output, 0 = human-readable */

/* ------------------------------------------------------------------ */
/* Structs                                                             */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)

typedef struct {
    uint32_t version;       /* 0=MBR, 1=GPT */
    uint32_t part_count;
    uint8_t  disk_id[16];   /* GPT: Disk GUID (LE) / MBR: Disk Signature(4) + padding */
    uint8_t  unknown[24];
} PTCache_Header;           /* 48 bytes */

typedef struct {
    uint32_t partition_number;  /* 0x00: パーティション番号 (1, 2, 3...) */
    uint32_t unknown_flag;      /* 0x04: 不明 (0x80 や 0x00 が入る) */
    uint64_t starting_offset;   /* 0x08: 開始オフセット (Bytes) */
    uint64_t partition_length;  /* 0x10: パーティションサイズ (Bytes) */
    uint64_t attributes;        /* 0x18: GPT 属性フラグ (8 bytes) */
    
    uint8_t  type_guid[16];     /* 0x20: GPT Type GUID */
    uint8_t  uniq_guid[16];     /* 0x30: GPT Unique GUID */
    uint8_t  unknown_padding[8];/* 0x40: パディング */
    uint8_t  name[72];          /* 0x48: UTF-16LE Partition Name */
} PTCache_GPT_Entry;            /* Total 144 bytes */


typedef struct {
    uint32_t partition_style;   /* 0x00: 0=MBR  */
    uint32_t unknown_04;        /* 0x04: 0x00  */
    uint64_t starting_offset;   /* 0x08: 開始オフセット (Bytes)  */
    uint64_t partition_length;  /* 0x10: パーティションサイズ (Bytes)  */
    uint64_t unknown_padding;   /* 0x18: ★この 8 バイトが不足していたため、以降がずれました  */

    /* --- ここから MBR 固有領域 (Offset 0x20) --- */
    uint8_t  boot_indicator;    /* 0x20: 0x80=Active, 0x00=Inactive  */
    uint8_t  start_chs[3];      /* 0x21: 開始 CHS  */
    uint8_t  partition_type;    /* 0x24: System ID (0x07=NTFS等)  */
    uint8_t  end_chs[3];        /* 0x25: 終了 CHS  */
    uint32_t start_lba_sector;  /* 0x28: 開始セクタ (LBA)  */
    uint32_t size_lba_sectors;  /* 0x2C: 総セクタ数  */

    uint32_t disk_signature;    /* 0x30: ディスク署名  */
    uint8_t  reserved[108];     /* 0x34: 残り  */
} PTCache_MBR_Entry;            /* Total 144 bytes  */

#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Known GPT Partition Type GUIDs (little-endian byte order)          */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t guid_bytes[16];
    const char   *description;
} GPT_TypeEntry;

static const GPT_TypeEntry gpt_types[] = {
    {{0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B},
      "EFI System Partition (ESP)"},
    {{0x16,0xE3,0xC9,0xE3,0x5C,0x0B,0xB8,0x4D,0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE},
      "Microsoft Reserved Partition (MSR)"},
    {{0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7},
      "Microsoft Basic Data Partition"},
    {{0xA4,0xBB,0x94,0xDE,0xD1,0x06,0x40,0x4D,0xA1,0x6A,0xBF,0xD5,0x01,0x79,0xD6,0xAC},
      "Windows Recovery Environment"},

    // Linux Filesystem Data (0FC63DAF-8483-4772-8E79-3D69D8477DE4)
    {{0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}, "Linux Filesystem"},
    // Linux Swap (0657FD6D-A4E8-4B33-A23F-B8F43360D707)
    {{0x6D, 0xFD, 0x57, 0x06, 0xE8, 0xA4, 0x33, 0x4B, 0xA2, 0x3F, 0xB8, 0xF4, 0x33, 0x60, 0xD7, 0x07}, "Linux Swap"},
    // Linux LVM (E6D6D379-F507-44C2-A23C-238F2A3AD000)
    {{0x79, 0xD3, 0xD6, 0xE6, 0x07, 0xF5, 0xC2, 0x44, 0xA2, 0x3C, 0x23, 0x8F, 0x2A, 0x3A, 0xD0, 0x00}, "Linux LVM"},
};

/* ------------------------------------------------------------------ */
/* Utility functions                                                   */
/* ------------------------------------------------------------------ */

static int hexval(char c)
{
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void print_guid(const uint8_t *g)
{
    printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g[3],g[2],g[1],g[0],
        g[5],g[4],
        g[7],g[6],
        g[8],g[9],
        g[10],g[11],g[12],g[13],g[14],g[15]);
}

static void extract_utf16_name(const uint8_t *p, char *out, size_t max_bytes)
{
    size_t j = 0;
    for (size_t i = 0; i + 1 < max_bytes; i += 2) {
        uint16_t ch = p[i] | (p[i+1] << 8);
        if (ch == 0x0000) break;
        if (ch < 0x80 && isprint(ch))
            out[j++] = (char)ch;
        else
            break;
        if (j >= ENTRY_NAME_MAXBYTES - 1) break;
    }
    out[j] = '\0';
}

static const char *match_gpt_type(const uint8_t *p)
{
    size_t n = sizeof(gpt_types) / sizeof(gpt_types[0]);
    for (size_t i = 0; i < n; i++) {
        if (memcmp(p, gpt_types[i].guid_bytes, 16) == 0)
            return gpt_types[i].description;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Input: extract hex blob from stdin                                  */
/* ------------------------------------------------------------------ */
/*
 * Auto-detects one of three input formats:
 *
 *  FORMAT_REGEDIT:  "PartitionTableCache"=hex:XX,XX,XX,...   (multi-line, comma-sep)
 *  FORMAT_REGDEC:   path|REG_BINARY|XX XX XX ...             (single line, space-sep)
 *  FORMAT_EVTX:     Event.EventData.Data.PartitionTable: XXXX...  (single line, no sep)
 *
 * In all cases hex bytes are collected into a buffer until end-of-format.
 */

#define FORMAT_NONE    0
#define FORMAT_REGEDIT 1   /* "PartitionTableCache"=hex:XX,XX,...  multi-line */
#define FORMAT_REGDEC  2   /* path|REG_BINARY|XX XX ...            one line   */
#define FORMAT_EVTX    3   /* Event.EventData.Data.PartitionTable: XXXX...    */
#define FORMAT_XML     4   /* <Data Name="PartitionTable">XXXX...</Data>      */

/* ------------------------------------------------------------------ */
/* Device label (set once per device, used in both text/CSV output)   */
/* ------------------------------------------------------------------ */

static char g_device_label[512] = "";

/* Build g_device_label from Manufacturer/Model/Serial strings.
 * Called by FORMAT_EVTX (TXT), FORMAT_XML, and FORMAT_REGDEC. */
static void build_device_label(const char *mfr, const char *model, const char *serial)
{
    /* Windows sometimes logs literal "NULL" for absent Manufacturer */
    if (strcmp(mfr, "NULL") == 0) mfr = "";

    if (mfr[0] || model[0]) {
        snprintf(g_device_label, sizeof(g_device_label), "%s%s%s%s%s%s",
                 mfr,
                 mfr[0] && model[0] ? " " : "", model,
                 serial[0] ? " [" : "", serial,
                 serial[0] ? "]" : "");
    } else {
        g_device_label[0] = '\0';
    }
}

/* Extract value after "prefix" on a line (e.g. "Event...Manufacturer: KIOXIA").
 * Writes result into buf (size buf_size), trims trailing whitespace. */
static void extract_line_value(const char *line, const char *prefix,
                                char *buf, size_t buf_size)
{
    const char *p = strstr(line, prefix);
    if (!p) return;
    p += strlen(prefix);
    while (*p == ' ' || *p == '\t') p++;
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < buf_size - 1)
        buf[i++] = *p++;
    buf[i] = '\0';
    while (i > 0 && (buf[i-1] == ' ' || buf[i-1] == '\t')) buf[--i] = '\0';
}

/* Extract value from XML: <Data Name="key">VALUE</Data>.
 * Copies VALUE into buf (size buf_size). Returns 1 if found and non-empty. */
static int extract_xml_value(const char *line, const char *name,
                              char *buf, size_t buf_size)
{
    char tag[256];
    snprintf(tag, sizeof(tag), "Name=\"%s\">", name);
    const char *p = strstr(line, tag);
    if (!p) return 0;
    p += strlen(tag);
    size_t i = 0;
    while (*p && *p != '<' && i < buf_size - 1)
        buf[i++] = *p++;
    buf[i] = '\0';
    return (i > 0);
}

/* Parse FORMAT_REGDEC path to extract Vendor, Product, Serial.
 * Path example:
 *   /ROOT/.../Disk&Ven_Realtek&Prod_RTL9210_NVME&Rev_1.00/012345678910&0/...
 * Result: "Realtek RTL9210_NVME [012345678910]" */
static void parse_regdec_label(const char *line)
{
    char ven[128]  = "";
    char prod[128] = "";
    char ser[128]  = "";
    const char *p;

    p = strstr(line, "Ven_");
    if (p) {
        p += 4;
        int i = 0;
        while (*p && *p != '&' && *p != '/' && *p != '\\' && i < (int)sizeof(ven) - 1)
            ven[i++] = *p++;
        ven[i] = '\0';
    }
    p = strstr(line, "Prod_");
    if (p) {
        p += 5;
        int i = 0;
        while (*p && *p != '&' && *p != '/' && *p != '\\' && i < (int)sizeof(prod) - 1)
            prod[i++] = *p++;
        prod[i] = '\0';
    }
    /* Serial is the path component immediately after Rev_x.xx/ */
    p = strstr(line, "Rev_");
    if (p) {
        while (*p && *p != '/') p++;
        if (*p == '/') p++;
        int i = 0;
        while (*p && *p != '&' && *p != '/' && *p != '|' && i < (int)sizeof(ser) - 1)
            ser[i++] = *p++;
        ser[i] = '\0';
    }
    build_device_label(ven, prod, ser);
}


static int buf_append(uint8_t **buf, size_t *buf_size, size_t *len, uint8_t byte)
{
    if (*len >= *buf_size) {
        *buf_size *= 2;
        uint8_t *tmp = realloc(*buf, *buf_size);
        if (!tmp) return 0;
        *buf = tmp;
    }
    (*buf)[(*len)++] = byte;
    return 1;
}

/* Scan pointer p, collect all XX XX / XX,XX / XXXX... hex bytes into buf. */
static int collect_hex(const char *p, uint8_t **buf, size_t *buf_size, size_t *len)
{
    while (*p) {
        if (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
            uint8_t byte = (uint8_t)((hexval(p[0]) << 4) | hexval(p[1]));
            if (!buf_append(buf, buf_size, len, byte)) return 0;
            p += 2;
        } else {
            p++;
        }
    }
    return 1;
}

int extract_partition_table_cache(uint8_t **out_buf, size_t *out_len)
{
    static char line[16384];   /* evtx PartitionTable can be ~1536 hex chars */
    static char last_hkey[1024] = "";  /* last [HKEY_...] section header seen */

    /*
     * Outer retry loop: Windows logs disconnect events with PartitionCount=0
     * and PartitionTable=[Empty].  collect_hex("[Empty]") produces len=0.
     * When that happens and we have not yet hit EOF, skip that event and
     * resume scanning for the next valid PartitionTable record.
     */
    for (;;)
    {
    uint8_t *buf      = malloc(INITIAL_BUF_SIZE);
    size_t   buf_size = INITIAL_BUF_SIZE;
    size_t   len      = 0;
    int      fmt      = FORMAT_NONE;

    /* Metadata accumulated from FORMAT_EVTX (TXT) and FORMAT_XML lines */
    char tmp_mfr[128]    = "";
    char tmp_model[128]  = "";
    char tmp_serial[128] = "";

    if (!buf) return 0;

    while (fgets(line, sizeof(line), stdin))
    {
        char *p = line;

        if (fmt == FORMAT_NONE)
        {
            /* Track [HKEY_...] section headers for FORMAT_REGEDIT label extraction */
            if (line[0] == '[') {
                strncpy(last_hkey, line, sizeof(last_hkey) - 1);
                last_hkey[sizeof(last_hkey) - 1] = '\0';
            }

            /* ---- FORMAT_REGEDIT: "PartitionTableCache"=hex: ---- */
            char *key = strstr(line, "\"PartitionTableCache\"=hex:");
            if (key) {
                fmt = FORMAT_REGEDIT;
                parse_regdec_label(last_hkey);  /* Ven_/Prod_ from [HKEY_...] header */
                p   = key + strlen("\"PartitionTableCache\"=hex:");
                if (!collect_hex(p, &buf, &buf_size, &len)) { free(buf); return 0; }
                continue;
            }

            /* ---- FORMAT_REGDEC: path|REG_BINARY|XX XX XX ... ---- */
            char *rb = strstr(line, "|REG_BINARY|");
            if (rb && strstr(line, "PartitionTableCache")) {
                fmt = FORMAT_REGDEC;
                parse_regdec_label(line);   /* extract Ven_/Prod_/Serial -> g_device_label */
                p   = rb + strlen("|REG_BINARY|");
                if (!collect_hex(p, &buf, &buf_size, &len)) { free(buf); return 0; }
                break;  /* single-line */
            }

            /* ---- FORMAT_EVTX (TXT): accumulate Manufacturer/Model/Serial ---- */
            extract_line_value(line, "Event.EventData.Data.Manufacturer:", tmp_mfr,    sizeof(tmp_mfr));
            extract_line_value(line, "Event.EventData.Data.Model:",         tmp_model,  sizeof(tmp_model));
            extract_line_value(line, "Event.EventData.Data.SerialNumber:",  tmp_serial, sizeof(tmp_serial));

            /* Trigger: Event.EventData.Data.PartitionTable: (not PartitionTableBytes) */
            char *et = strstr(line, "Event.EventData.Data.PartitionTable:");
            if (et && !strstr(line, "PartitionTableBytes")) {
                fmt = FORMAT_EVTX;
                build_device_label(tmp_mfr, tmp_model, tmp_serial);
                p   = et + strlen("Event.EventData.Data.PartitionTable:");
                while (*p && isspace((unsigned char)*p)) p++;
                if (!collect_hex(p, &buf, &buf_size, &len)) { free(buf); return 0; }
                break;  /* single-line */
            }

            /* ---- FORMAT_XML: <Data Name="...">VALUE</Data> lines ---- */
            {
                char xval[256] = "";
                if (extract_xml_value(line, "Manufacturer", xval, sizeof(xval)) && xval[0])
                    strncpy(tmp_mfr,    xval, sizeof(tmp_mfr)    - 1);
                if (extract_xml_value(line, "Model",        xval, sizeof(xval)) && xval[0])
                    strncpy(tmp_model,  xval, sizeof(tmp_model)  - 1);
                if (extract_xml_value(line, "SerialNumber", xval, sizeof(xval)) && xval[0])
                    strncpy(tmp_serial, xval, sizeof(tmp_serial) - 1);
            }

            /* Trigger: Name="PartitionTable" (not PartitionTableBytes) */
            if (strstr(line, "\"PartitionTable\"") && !strstr(line, "PartitionTableBytes"))
            {
                char xval[32768] = "";
                if (extract_xml_value(line, "PartitionTable", xval, sizeof(xval)) && xval[0]) {
                    fmt = FORMAT_XML;
                    build_device_label(tmp_mfr, tmp_model, tmp_serial);
                    if (!collect_hex(xval, &buf, &buf_size, &len)) { free(buf); return 0; }
                    break;  /* single-line */
                }
            }

            continue;  /* no trigger found yet */
        }
        else if (fmt == FORMAT_REGEDIT)
        {
            /* continuation lines: must start with whitespace then hex */
            char *tmp = line;
            while (*tmp && isspace((unsigned char)*tmp)) tmp++;
            if (!isxdigit((unsigned char)tmp[0])) break;   /* end of hex block */
            if (!collect_hex(tmp, &buf, &buf_size, &len)) { free(buf); return 0; }
        }
        /* All other formats are single-line, loop exits via break */
    }

    if (len == 0) {
        free(buf);
        if (feof(stdin)) return 0;  /* true EOF — no more events */
        continue;                   /* [Empty] PartitionTable — skip and retry */
    }

    *out_buf = buf;
    *out_len = len;
    return 1;

    } /* end outer retry loop */
}


/* ------------------------------------------------------------------ */
/* CSV helpers                                                         */
/* ------------------------------------------------------------------ */

/* Write GUID as string into buf (must be >= 37 bytes) */
static void guid_to_str(const uint8_t *g, char *buf)
{
    sprintf(buf,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g[3],g[2],g[1],g[0],
        g[5],g[4],
        g[7],g[6],
        g[8],g[9],
        g[10],g[11],g[12],g[13],g[14],g[15]);
}

/* Print a CSV field, quoting if it contains comma/quote/newline */
static void csv_field(const char *s)
{
    int need_quote = 0;
    for (const char *p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            need_quote = 1; break;
        }
    }
    if (need_quote) {
        putchar('"');
        for (const char *p = s; *p; p++) {
            if (*p == '"') putchar('"');  /* escape " as "" */
            putchar(*p);
        }
        putchar('"');
    } else {
        fputs(s, stdout);
    }
}

/* Print CSV header (called once) */
static void print_csv_header(void)
{
    printf("Device,DeviceLabel,DiskType,DiskID,PartNo,StartOffset,Size,Attributes,TypeGUID,TypeName,UniqueGUID,Name\n");
}

/* Print one CSV row for a GPT partition (Extended with LBA info) */
static void print_csv_gpt_row(int device, const char *disk_guid,
                              uint32_t part_no,
                              const PTCache_GPT_Entry *entry)
{
    char type_guid_str[37];
    char uniq_guid_str[37];
    char name[ENTRY_NAME_MAXBYTES];
    const char *type_name;

    guid_to_str(entry->type_guid, type_guid_str);
    guid_to_str(entry->uniq_guid, uniq_guid_str);
    extract_utf16_name(entry->name, name, sizeof(entry->name));
    type_name = match_gpt_type(entry->type_guid);

    printf("%d,", device);
    csv_field(g_device_label); putchar(',');
    printf("GPT,");
    csv_field(disk_guid);  putchar(',');
    printf("%u,", part_no);
    printf("%llu,", (unsigned long long)entry->starting_offset);
    printf("%llu,", (unsigned long long)entry->partition_length);
    printf("0x%08llX,", (unsigned long long)entry->attributes);
    csv_field(type_guid_str); putchar(',');
    csv_field(type_name ? type_name : ""); putchar(',');
    csv_field(uniq_guid_str); putchar(',');
    csv_field(name[0] ? name : "");
    putchar('\n');
}



static const char* match_mbr_type(uint8_t type)
{
    switch (type) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32M";
        case 0x05: return "Extended Partition (CHS)";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/exFAT";
        case 0x0B: return "FAT32 (CHS)";
        case 0x0C: return "FAT32 (LBA)";
        case 0x0E: return "FAT16 (LBA)";
        case 0x0F: return "Extended Partition (LBA)";
        case 0x11: return "Hidden FAT12";
        case 0x14: return "Hidden FAT16 <32M";
        case 0x16: return "Hidden FAT16";
        case 0x17: return "Hidden NTFS";
        case 0x27: return "Windows Recovery Environment";
        case 0xEE: return "GPT Protective MBR";
        case 0xEF: return "EFI System Partition";
        default:   return "Unknown";
    }
}


/* Print one CSV row for an MBR partition */
static void print_csv_mbr_row(int device, const char *disk_sig,
                               uint32_t part_no,
                               const PTCache_MBR_Entry *entry)
{
    if (entry->starting_offset == 0 && entry->partition_length == 0) return;

    const char *type_name = match_mbr_type(entry->partition_type);

    printf("%d,", device);
    csv_field(g_device_label); putchar(',');
    printf("MBR,");
    csv_field(disk_sig); putchar(',');
    printf("%u,", part_no);
    printf("%llu,", (unsigned long long)entry->starting_offset);  /* StartOffset */
    printf("%llu,", (unsigned long long)entry->partition_length);  /* Size        */
    printf("0x%02X,", entry->boot_indicator);                      /* Attributes = Boot Indicator (0x80=Active) */
    printf("0x%02X,", entry->partition_type);                      /* TypeGUID   = MBR System ID  */
    csv_field(type_name); putchar(',');                             /* TypeName                    */
    putchar(',');                                                   /* UniqueGUID: N/A for MBR     */
                                                                    /* Name:       N/A for MBR     */
    putchar('\n');
}

/* ------------------------------------------------------------------ */
/* GPT: print partition entries                                        */
/* ------------------------------------------------------------------ */

static void print_gpt_entries(const uint8_t *buf, size_t len, uint32_t count)
{
    for (uint32_t e = 0; e < count; e++)
    {
        size_t offset = PTCACHE_HEADER_SIZE + (size_t)e * PTCACHE_ENTRY_SIZE;
        if (offset + PTCACHE_ENTRY_SIZE > len) {
            printf("  [WARNING] entry %u out of bounds\n", e + 1);
            break;
        }

        const PTCache_GPT_Entry *entry =
            (const PTCache_GPT_Entry *)(buf + offset);

        const char *type_name = match_gpt_type(entry->type_guid);

        // パーティション番号と基本情報の出力
        printf("[%u] Partition Number: %u\n", e + 1, e + 1);

        // LBA情報の出力 (バイト単位からセクタ単位/GB単位へ計算)
        uint64_t start_lba = entry->starting_offset / 512;
        double size_gb = (double)entry->partition_length / (1024 * 1024 * 1024);

        printf("  Starting Offset: 0x%016llx (LBA: %llu)\n", 
               (unsigned long long)entry->starting_offset, (unsigned long long)start_lba);
        printf("  Partition Size:  0x%016llx (%.2f GB)\n", 
               (unsigned long long)entry->partition_length, size_gb);
        
        printf("  Attributes:      0x%08llx\n", entry->attributes);




        // GUID情報の出力
        printf("  Type GUID:       ");
        print_guid(entry->type_guid);
        printf(" (%s)\n", type_name ? type_name : "unknown");

        printf("  Unique GUID:     ");
        print_guid(entry->uniq_guid);
        printf("\n");

        // 名前の出力
        char name[ENTRY_NAME_MAXBYTES];
        extract_utf16_name(entry->name, name, sizeof(entry->name));
        printf("  Cached Name:     %s\n\n",
               name[0] ? name : "(not present)");
    }
}



/* ------------------------------------------------------------------ */
/* MBR: print partition entries (internal structure TBD)              */
/* ------------------------------------------------------------------ */


static void print_mbr_entries(const uint8_t *buf, size_t len, uint32_t count)
{   
    for (uint32_t e = 0; e < count; e++)
    {
        size_t offset = PTCACHE_HEADER_SIZE + (size_t)e * PTCACHE_ENTRY_SIZE;
        if (offset + PTCACHE_ENTRY_SIZE > len) break;

        const PTCache_MBR_Entry *entry = (const PTCache_MBR_Entry *)(buf + offset);

        if (entry->starting_offset == 0 && entry->partition_length == 0) continue;

        printf("[%u] MBR Partition\n", e + 1);

        uint64_t start_lba = entry->starting_offset / 512;
        double size_gb = (double)entry->partition_length / (1024 * 1024 * 1024);

        printf("  Starting Offset: 0x%016llx (LBA: %llu)\n", 
               (unsigned long long)entry->starting_offset, (unsigned long long)start_lba);
        printf("  Partition Size:  0x%016llx (%.2f GB)\n", 
               (unsigned long long)entry->partition_length, size_gb);

        /* match_mbr_type() を使って 0x07 を "NTFS/exFAT" と表示 */
        const char *type_name = match_mbr_type(entry->partition_type);
        printf("  System ID:       0x%02X (%s)\n", entry->partition_type, type_name);
        
        printf("  Boot Indicator:  0x%02X %s\n", 
               entry->boot_indicator, (entry->boot_indicator & 0x80) ? "(Active)" : "");

        printf("  Disk Signature:  %08X\n\n", entry->disk_signature);
    }       
}




/* ------------------------------------------------------------------ */
/* Main parser                                                         */
/* ------------------------------------------------------------------ */

static void print_partition_table_cache(const uint8_t *buf, size_t len, int device)
{
    if (len < PTCACHE_HEADER_SIZE) {
        fprintf(stderr, "Too small to parse (%zu bytes).\n", len);
        return;
    }

    const PTCache_Header *hdr = (const PTCache_Header *)buf;
    uint32_t version    = hdr->version;
    uint32_t count      = hdr->part_count;
    size_t   expected   = PTCACHE_HEADER_SIZE + (size_t)count * PTCACHE_ENTRY_SIZE;

    /* ---------- CSV mode ---------- */
    if (g_csv_mode)
    {
        char disk_id_str[40];

        if (version == 1) {
            guid_to_str(hdr->disk_id, disk_id_str);
        } else if (version == 0) {
            uint32_t sig = hdr->disk_id[0] | (hdr->disk_id[1] << 8)
                         | (hdr->disk_id[2] << 16) | (hdr->disk_id[3] << 24);
            sprintf(disk_id_str, "%08X", sig);
        } else {
            sprintf(disk_id_str, "unknown");
        }

        for (uint32_t e = 0; e < count; e++) {
            size_t offset = PTCACHE_HEADER_SIZE + (size_t)e * PTCACHE_ENTRY_SIZE;
            if (offset + PTCACHE_ENTRY_SIZE > len) break;

            if (version == 1) {
                const PTCache_GPT_Entry *entry =
                    (const PTCache_GPT_Entry *)(buf + offset);
                print_csv_gpt_row(device, disk_id_str, e + 1, entry);
            } else {
                const PTCache_MBR_Entry *entry =
                    (const PTCache_MBR_Entry *)(buf + offset);
                print_csv_mbr_row(device, disk_id_str, e + 1, entry);
            }
        }
        return;
    }

    /* ---------- Human-readable mode ---------- */

    if (g_device_label[0])
        printf("\n=== Device #%d  [%s] ===\n", device, g_device_label);
    else
        printf("\n=== Device #%d ===\n", device);
    printf("PartitionTableCache Summary\n");
    printf("-----------------------------------------\n");
    printf("Version:          %u (%s)\n", version,
           version == 1 ? "GPT" : version == 0 ? "MBR" : "Unknown");
    printf("Partition Count:  %u\n", count);
    printf("Expected Size:    %zu bytes\n", expected);
    printf("Actual Size:      %zu bytes\n", len);

    if (expected != len)
        printf("[WARNING] size mismatch: expected %zu, got %zu\n", expected, len);

    if (version == 1) {
        printf("Disk GUID:        ");
        print_guid(hdr->disk_id);
        printf("\n");
    } else if (version == 0) {
        uint32_t sig = hdr->disk_id[0] | (hdr->disk_id[1] << 8)
                     | (hdr->disk_id[2] << 16) | (hdr->disk_id[3] << 24);
        printf("Disk Signature:   %08X\n", sig);
    }

    printf("\nDetected Partitions:\n");
    printf("-----------------------------------------\n");

    if (version == 1)
        print_gpt_entries(buf, len, count);
    else if (version == 0)
        print_mbr_entries(buf, len, count);
    else
        printf("Unknown version, cannot parse entries.\n");
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    /* Parse options */
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--csv") == 0) || (strcmp(argv[i], "-c") == 0)) {
            g_csv_mode = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Usage: %s [-csv]\n", argv[0]);
            return 1;
        }
    }

    uint8_t *buf;
    size_t   len;
    int      index = 0;

    if (g_csv_mode)
        print_csv_header();

    while (extract_partition_table_cache(&buf, &len))
    {
        ++index;
        print_partition_table_cache(buf, len, index);
        free(buf);
    }

    if (index == 0)
    {
        fprintf(stderr, "PartitionTableCache not found\n");
        return 1;
    }

    return 0;
}
