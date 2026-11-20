/*
 * usn_deleted.c - USN Journal ($UsnJrnl:$J) Analyzer
 *
 * Purpose: Accurately determine deletion time of deleted files
 *          by analyzing NTFS USN Journal records.
 *
 * Input:   Raw $J stream extracted from $UsnJrnl:$J
 *          (e.g., via: icat -o <offset> image.e01 <$UsnJrnl MFT#>-128-1 > usnjournal.bin)
 *
 * Usage:
 *   ./usn_deleted -j usnjournal.bin -n "target_filename"
 *   ./usn_deleted -j usnjournal.bin -r <FileReferenceNumber(decimal)>
 *   ./usn_deleted -j usnjournal.bin -n "target.txt" -c   (CSV output)
 *   ./usn_deleted -j usnjournal.bin --all                (全レコードダンプ)
 *   ./usn_deleted -j usnjournal.bin -n "target.txt" -v   (詳細モード)
 *
 * Build:
 *   gcc -o usn_deleted usn_deleted.c -Wall -O2
 *
 * Reference:
 *   USN_RECORD_V2: https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_record_v2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <getopt.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Data Structures                                                      */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)

/* USN Record V2 (most common) */
typedef struct {
    uint32_t  RecordLength;
    uint16_t  MajorVersion;
    uint16_t  MinorVersion;
    uint64_t  FileReferenceNumber;       /* lower 48 bits = MFT#, upper 16 bits = seq */
    uint64_t  ParentFileReferenceNumber;
    int64_t   Usn;                       /* Update Sequence Number */
    int64_t   TimeStamp;                 /* FILETIME (100ns intervals since 1601-01-01) */
    uint32_t  Reason;
    uint32_t  SourceInfo;
    uint32_t  SecurityId;
    uint32_t  FileAttributes;
    uint16_t  FileNameLength;            /* bytes (not chars) */
    uint16_t  FileNameOffset;            /* from start of record */
    /* FileName follows (UTF-16LE, NOT null-terminated) */
} USN_RECORD_V2;

#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* USN Reason Flags                                                     */
/* ------------------------------------------------------------------ */

#define USN_REASON_DATA_OVERWRITE        0x00000001
#define USN_REASON_DATA_EXTEND           0x00000002
#define USN_REASON_DATA_TRUNCATION       0x00000004
#define USN_REASON_NAMED_DATA_OVERWRITE  0x00000010
#define USN_REASON_NAMED_DATA_EXTEND     0x00000020
#define USN_REASON_NAMED_DATA_TRUNCATION 0x00000040
#define USN_REASON_FILE_CREATE           0x00000100
#define USN_REASON_FILE_DELETE           0x00000200
#define USN_REASON_EA_CHANGE             0x00000400
#define USN_REASON_SECURITY_CHANGE       0x00000800
#define USN_REASON_RENAME_OLD_NAME       0x00001000
#define USN_REASON_RENAME_NEW_NAME       0x00002000
#define USN_REASON_INDEXABLE_CHANGE      0x00004000
#define USN_REASON_BASIC_INFO_CHANGE     0x00008000
#define USN_REASON_HARD_LINK_CHANGE      0x00010000
#define USN_REASON_COMPRESSION_CHANGE    0x00020000
#define USN_REASON_ENCRYPTION_CHANGE     0x00040000
#define USN_REASON_OBJECT_ID_CHANGE      0x00080000
#define USN_REASON_REPARSE_POINT_CHANGE  0x00100000
#define USN_REASON_STREAM_CHANGE         0x00200000
#define USN_REASON_TRANSACTED_CHANGE     0x00800000
#define USN_REASON_INTEGRITY_CHANGE      0x00800000
#define USN_REASON_CLOSE                 0x80000000

/* File Attributes */
#define FILE_ATTRIBUTE_READONLY          0x0001
#define FILE_ATTRIBUTE_HIDDEN            0x0002
#define FILE_ATTRIBUTE_SYSTEM            0x0004
#define FILE_ATTRIBUTE_DIRECTORY         0x0010
#define FILE_ATTRIBUTE_ARCHIVE           0x0020
#define FILE_ATTRIBUTE_DEVICE            0x0040
#define FILE_ATTRIBUTE_NORMAL            0x0080
#define FILE_ATTRIBUTE_TEMPORARY         0x0100
#define FILE_ATTRIBUTE_SPARSE_FILE       0x0200
#define FILE_ATTRIBUTE_REPARSE_POINT     0x0400
#define FILE_ATTRIBUTE_COMPRESSED        0x0800
#define FILE_ATTRIBUTE_OFFLINE           0x1000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x2000
#define FILE_ATTRIBUTE_ENCRYPTED         0x4000

/* ------------------------------------------------------------------ */
/* Global Options                                                       */
/* ------------------------------------------------------------------ */

static int  opt_csv      = 0;
static int  opt_verbose  = 0;
static int  opt_all      = 0;
static int  opt_delete_only = 0;

/* ------------------------------------------------------------------ */
/* FILETIME → UTC string                                               */
/* ------------------------------------------------------------------ */

static void filetime_to_str(int64_t ft, char *buf, size_t len) {
    if (ft == 0) {
        snprintf(buf, len, "N/A");
        return;
    }
    /* FILETIME: 100ns intervals since 1601-01-01 00:00:00 UTC */
    uint64_t uft = (uint64_t)ft;
    /* seconds since 1601-01-01 */
    uint64_t sec100ns = uft % 10000000ULL;
    uint64_t secs     = uft / 10000000ULL;
    /* Unix epoch starts 1970-01-01 = 134774 days after 1601-01-01 */
    uint64_t unix_secs = secs - 11644473600ULL;
    uint32_t sub_usec  = (uint32_t)(sec100ns / 10);  /* microseconds */

    time_t t = (time_t)unix_secs;
    struct tm *tm = gmtime(&t);
    if (!tm) {
        snprintf(buf, len, "INVALID");
        return;
    }
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d.%06u UTC",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec,
             sub_usec);
}

/* ------------------------------------------------------------------ */
/* Reason flags → string                                               */
/* ------------------------------------------------------------------ */

static void reason_to_str(uint32_t reason, char *buf, size_t len) {
    buf[0] = '\0';
    size_t off = 0;

    struct { uint32_t flag; const char *name; } flags[] = {
        { USN_REASON_FILE_CREATE,           "FILE_CREATE"           },
        { USN_REASON_FILE_DELETE,           "FILE_DELETE"           },
        { USN_REASON_DATA_OVERWRITE,        "DATA_OVERWRITE"        },
        { USN_REASON_DATA_EXTEND,           "DATA_EXTEND"           },
        { USN_REASON_DATA_TRUNCATION,       "DATA_TRUNCATION"       },
        { USN_REASON_RENAME_OLD_NAME,       "RENAME_OLD_NAME"       },
        { USN_REASON_RENAME_NEW_NAME,       "RENAME_NEW_NAME"       },
        { USN_REASON_SECURITY_CHANGE,       "SECURITY_CHANGE"       },
        { USN_REASON_BASIC_INFO_CHANGE,     "BASIC_INFO_CHANGE"     },
        { USN_REASON_HARD_LINK_CHANGE,      "HARD_LINK_CHANGE"      },
        { USN_REASON_COMPRESSION_CHANGE,    "COMPRESSION_CHANGE"    },
        { USN_REASON_ENCRYPTION_CHANGE,     "ENCRYPTION_CHANGE"     },
        { USN_REASON_OBJECT_ID_CHANGE,      "OBJECT_ID_CHANGE"      },
        { USN_REASON_EA_CHANGE,             "EA_CHANGE"             },
        { USN_REASON_STREAM_CHANGE,         "STREAM_CHANGE"         },
        { USN_REASON_REPARSE_POINT_CHANGE,  "REPARSE_POINT_CHANGE"  },
        { USN_REASON_NAMED_DATA_OVERWRITE,  "NAMED_DATA_OVERWRITE"  },
        { USN_REASON_NAMED_DATA_EXTEND,     "NAMED_DATA_EXTEND"     },
        { USN_REASON_NAMED_DATA_TRUNCATION, "NAMED_DATA_TRUNCATION" },
        { USN_REASON_INDEXABLE_CHANGE,      "INDEXABLE_CHANGE"      },
        { USN_REASON_CLOSE,                 "CLOSE"                 },
        { 0, NULL }
    };

    for (int i = 0; flags[i].name; i++) {
        if (reason & flags[i].flag) {
            if (off > 0 && off < len - 2)
                buf[off++] = '|';
            size_t nlen = strlen(flags[i].name);
            if (off + nlen < len - 1) {
                memcpy(buf + off, flags[i].name, nlen);
                off += nlen;
                buf[off] = '\0';
            }
        }
    }
    if (off == 0)
        snprintf(buf, len, "0x%08X", reason);
}

/* ------------------------------------------------------------------ */
/* File Attributes → short string                                      */
/* ------------------------------------------------------------------ */

static void attr_to_str(uint32_t attr, char *buf, size_t len) {
    char tmp[256] = "";
    if (attr & FILE_ATTRIBUTE_DIRECTORY)  strcat(tmp, "D");
    else                                   strcat(tmp, "-");
    if (attr & FILE_ATTRIBUTE_READONLY)   strcat(tmp, "R");
    if (attr & FILE_ATTRIBUTE_HIDDEN)     strcat(tmp, "H");
    if (attr & FILE_ATTRIBUTE_SYSTEM)     strcat(tmp, "S");
    if (attr & FILE_ATTRIBUTE_ARCHIVE)    strcat(tmp, "A");
    if (attr & FILE_ATTRIBUTE_TEMPORARY)  strcat(tmp, "T");
    if (attr & FILE_ATTRIBUTE_COMPRESSED) strcat(tmp, "C");
    if (attr & FILE_ATTRIBUTE_ENCRYPTED)  strcat(tmp, "E");
    snprintf(buf, len, "%s", tmp);
}

/* ------------------------------------------------------------------ */
/* UTF-16LE → UTF-8 (simple BMP-only conversion)                      */
/* ------------------------------------------------------------------ */

static int utf16le_to_utf8(const uint8_t *src, int src_bytes,
                            char *dst, int dst_len) {
    int di = 0;
    for (int i = 0; i + 1 < src_bytes && di < dst_len - 4; i += 2) {
        uint16_t wc = (uint16_t)src[i] | ((uint16_t)src[i+1] << 8);
        if (wc == 0) break;
        if (wc < 0x80) {
            dst[di++] = (char)wc;
        } else if (wc < 0x800) {
            dst[di++] = (char)(0xC0 | (wc >> 6));
            dst[di++] = (char)(0x80 | (wc & 0x3F));
        } else {
            dst[di++] = (char)(0xE0 | (wc >> 12));
            dst[di++] = (char)(0x80 | ((wc >> 6) & 0x3F));
            dst[di++] = (char)(0x80 | (wc & 0x3F));
        }
    }
    dst[di] = '\0';
    return di;
}

/* ------------------------------------------------------------------ */
/* Case-insensitive filename match (UTF-8)                             */
/* ------------------------------------------------------------------ */

static int filename_match(const char *record_name, const char *target) {
    /* Simple case-insensitive ASCII comparison; sufficient for most filenames */
    size_t rlen = strlen(record_name);
    size_t tlen = strlen(target);
    if (rlen != tlen) return 0;
    for (size_t i = 0; i < rlen; i++) {
        if (tolower((unsigned char)record_name[i]) !=
            tolower((unsigned char)target[i])) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Print single USN record                                             */
/* ------------------------------------------------------------------ */

static void print_record(const USN_RECORD_V2 *rec, const char *filename,
                         uint64_t file_offset, int is_delete_event) {
    char ts[64];
    char reason_str[512];
    char attr_str[32];
    uint64_t mft_num = rec->FileReferenceNumber & 0x0000FFFFFFFFFFFFULL;
    uint16_t mft_seq = (uint16_t)(rec->FileReferenceNumber >> 48);
    uint64_t par_mft = rec->ParentFileReferenceNumber & 0x0000FFFFFFFFFFFFULL;

    filetime_to_str(rec->TimeStamp, ts, sizeof(ts));
    reason_to_str(rec->Reason, reason_str, sizeof(reason_str));
    attr_to_str(rec->FileAttributes, attr_str, sizeof(attr_str));

    if (opt_csv) {
        /* CSV format: Timestamp,USN,MFT#,Seq,ParentMFT#,Reason,Attr,Filename,FileOffset */
        printf("%s,%" PRId64 ",%" PRIu64 ",%u,%" PRIu64 ",%s,%s,%s,0x%016" PRIX64 "\n",
               ts,
               rec->Usn,
               mft_num, mft_seq, par_mft,
               reason_str,
               attr_str,
               filename,
               file_offset);
    } else {
        /* Human-readable format */
        if (is_delete_event) {
            printf("╔══════════════════════════════════════════════════════════════╗\n");
            printf("║  *** 削除イベント検出 (FILE_DELETE) ***                     ║\n");
            printf("╠══════════════════════════════════════════════════════════════╣\n");
        } else {
            printf("──────────────────────────────────────────────────────────────\n");
        }

        printf("  タイムスタンプ : %s\n", ts);
        printf("  USN            : %" PRId64 " (0x%016" PRIX64 ")\n",
               rec->Usn, (uint64_t)rec->Usn);
        printf("  ファイル名     : %s\n", filename);
        printf("  MFT番号        : %" PRIu64 " (seq=%u)\n", mft_num, mft_seq);
        printf("  親MFT番号      : %" PRIu64 "\n", par_mft);
        printf("  操作           : %s\n", reason_str);
        printf("  属性           : %s (0x%08X)\n", attr_str, rec->FileAttributes);
        printf("  ジャーナルOffset: 0x%016" PRIX64 "\n", file_offset);

        if (is_delete_event) {
            printf("╚══════════════════════════════════════════════════════════════╝\n");
        }
        printf("\n");
    }
}

/* ------------------------------------------------------------------ */
/* Main parsing loop                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t  file_ref;
    char      filename[512];
    int64_t   timestamp;
    int64_t   usn;
    uint32_t  reason;
    uint64_t  file_offset;
} MatchRecord;

#define MAX_MATCHES 65536

static MatchRecord *g_matches = NULL;
static int          g_match_count = 0;

static void parse_usn_journal(FILE *fp,
                               const char *target_name,
                               uint64_t    target_ref,   /* 0 = ignore */
                               int search_by_ref) {
    uint8_t  buf[65536];
    uint64_t file_offset = 0;
    size_t   bytes_read;
    size_t   buf_used = 0;

    /* Allocate match buffer */
    g_matches = (MatchRecord *)malloc(sizeof(MatchRecord) * MAX_MATCHES);
    if (!g_matches) { perror("malloc"); exit(1); }
    g_match_count = 0;

    uint64_t total_records  = 0;
    uint64_t sparse_skipped = 0;

    printf("# USN Journal 解析開始...\n\n");

    /* Print CSV header */
    if (opt_csv) {
        printf("Timestamp,USN,MFT_Num,MFT_Seq,Parent_MFT_Num,Reason,Attributes,Filename,Journal_Offset\n");
    }

    while ((bytes_read = fread(buf + buf_used, 1,
                               sizeof(buf) - buf_used, fp)) > 0 ||
           buf_used > 0) {

        if (bytes_read == 0 && buf_used == 0)
            break;

        if (!bytes_read) break;  /* EOF: 残りバイトは不完全レコード → 終了 */

        buf_used += bytes_read;
        size_t pos = 0;

        while (pos + sizeof(USN_RECORD_V2) <= buf_used) {

            /* Skip sparse (zero-filled) regions in 8-byte steps */
            if (*(uint32_t *)(buf + pos) == 0) {
                size_t skip = 0;
                while (pos + skip + 3 < buf_used &&
                       *(uint32_t *)(buf + pos + skip) == 0) {
                    skip += 8;
                    sparse_skipped++;
                }
                file_offset += skip;
                pos += skip;
                continue;
            }

            USN_RECORD_V2 *rec = (USN_RECORD_V2 *)(buf + pos);

            /* Sanity checks */
            if (rec->RecordLength < sizeof(USN_RECORD_V2) ||
                rec->RecordLength > 65536 ||
                (rec->RecordLength & 7) != 0) {
                /* Misaligned or corrupt — advance 8 bytes */
                file_offset += 8;
                pos += 8;
                continue;
            }

            /* Wait for full record in buffer */
            if (pos + rec->RecordLength > buf_used) break;

            /* Only V2 records supported */
            if (rec->MajorVersion != 2) {
                file_offset += rec->RecordLength;
                pos += rec->RecordLength;
                continue;
            }

            total_records++;

            /* Extract filename (UTF-16LE) */
            char fname_utf8[512] = "";
            if (rec->FileNameOffset + rec->FileNameLength <=
                rec->RecordLength && rec->FileNameLength > 0) {
                const uint8_t *fn_ptr =
                    (const uint8_t *)rec + rec->FileNameOffset;
                utf16le_to_utf8(fn_ptr, rec->FileNameLength,
                                fname_utf8, sizeof(fname_utf8));
            }

            uint64_t cur_offset = file_offset + pos;
                /* NOTE: file_offset is base of current buffer fill,
                         pos is offset within buffer */

            /* Determine if this record matches the search criteria */
            int match = 0;
            if (opt_all) {
                match = 1;
            } else if (search_by_ref) {
                uint64_t rec_ref =
                    rec->FileReferenceNumber & 0x0000FFFFFFFFFFFFULL;
                if (rec_ref == target_ref) match = 1;
            } else if (target_name) {
                if (filename_match(fname_utf8, target_name)) match = 1;
            }

            if (match) {
                int is_del = (rec->Reason & USN_REASON_FILE_DELETE) != 0;

                /* Filter: deletion only? */
                if (opt_delete_only && !is_del) {
                    file_offset += 0; /* keep tracking */
                    pos += rec->RecordLength;
                    continue;
                }

                print_record(rec, fname_utf8, cur_offset, is_del);

                /* Store in match buffer for summary */
                if (g_match_count < MAX_MATCHES) {
                    MatchRecord *mr = &g_matches[g_match_count++];
                    mr->file_ref   = rec->FileReferenceNumber;
                    strncpy(mr->filename, fname_utf8,
                            sizeof(mr->filename) - 1);
                    mr->filename[sizeof(mr->filename)-1] = '\0';
                    mr->timestamp  = rec->TimeStamp;
                    mr->usn        = rec->Usn;
                    mr->reason     = rec->Reason;
                    mr->file_offset = cur_offset;
                }
            }

            pos         += rec->RecordLength;
        }

        /* Shift remaining bytes to front of buffer */
        if (pos > 0 && pos < buf_used) {
            memmove(buf, buf + pos, buf_used - pos);
            file_offset += pos;
            buf_used    -= pos;
        } else if (pos >= buf_used) {
            file_offset += pos;
            buf_used     = 0;
        }
    }

    if (!opt_csv) {
        printf("──────────────────────────────────────────────────────────────\n");
        printf("# 解析完了\n");
        printf("  総レコード数     : %" PRIu64 "\n", total_records);
        printf("  スパース領域スキップ: %" PRIu64 " (x8bytes)\n", sparse_skipped);
        printf("  マッチレコード数 : %d\n", g_match_count);
    }
}

/* ------------------------------------------------------------------ */
/* Summary: deletion timeline for the matched file                    */
/* ------------------------------------------------------------------ */

static void print_summary(void) {
    if (opt_csv) return;  /* CSV mode: no summary */
    if (g_match_count == 0) {
        printf("\n[!] 対象ファイルのレコードが見つかりませんでした。\n");
        return;
    }

    /* Find key events */
    MatchRecord *ev_create  = NULL;
    MatchRecord *ev_delete  = NULL;  /* first FILE_DELETE */
    MatchRecord *ev_close_after_delete = NULL;
    MatchRecord *ev_rename_old = NULL;
    MatchRecord *ev_rename_new = NULL;
    MatchRecord *ev_last    = NULL;

    /* ゴミ箱移動判定用 */
    int moved_to_recycle = 0;

    for (int i = 0; i < g_match_count; i++) {
        MatchRecord *mr = &g_matches[i];
        if (mr->reason & USN_REASON_FILE_CREATE && !ev_create)
            ev_create = mr;
        if (mr->reason & USN_REASON_FILE_DELETE && !ev_delete)
            ev_delete = mr;
        if (mr->reason & USN_REASON_RENAME_OLD_NAME && !ev_rename_old)
            ev_rename_old = mr;
        if (mr->reason & USN_REASON_RENAME_NEW_NAME && !ev_rename_new)
            ev_rename_new = mr;
        ev_last = mr;

        /* ゴミ箱へのリネームパターン検出 ($Recycle.Bin or RECYCLER) */
        if ((mr->reason & USN_REASON_RENAME_NEW_NAME) &&
            (strstr(mr->filename, "$R") == mr->filename ||
             strstr(mr->filename, "$I") == mr->filename)) {
            moved_to_recycle = 1;
        }

        /* CLOSE directly after FILE_DELETE */
        if (ev_delete && !ev_close_after_delete &&
            (mr->reason & USN_REASON_CLOSE) &&
            (mr->reason & USN_REASON_FILE_DELETE)) {
            ev_close_after_delete = mr;
        }
    }

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║              削除日時分析サマリー                           ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");

    char ts[64];

    if (ev_create) {
        filetime_to_str(ev_create->timestamp, ts, sizeof(ts));
        printf("  [作成]     %s\n", ts);
        printf("             USN: %" PRId64 "\n", ev_create->usn);
    }

    if (ev_rename_old) {
        filetime_to_str(ev_rename_old->timestamp, ts, sizeof(ts));
        printf("  [リネーム前] %s\n", ts);
    }
    if (ev_rename_new) {
        filetime_to_str(ev_rename_new->timestamp, ts, sizeof(ts));
        printf("  [リネーム後] %s  (%s)\n", ts, ev_rename_new->filename);
        if (moved_to_recycle) {
            printf("             ↑ ゴミ箱への移動を検出\n");
        }
    }

    if (ev_delete) {
        filetime_to_str(ev_delete->timestamp, ts, sizeof(ts));
        printf("\n");
        printf("  ★[削除]    %s\n", ts);
        printf("             USN       : %" PRId64 "\n", ev_delete->usn);
        printf("             Offset    : 0x%016" PRIX64 "\n", ev_delete->file_offset);
        uint64_t mft = ev_delete->file_ref & 0x0000FFFFFFFFFFFFULL;
        uint16_t seq = (uint16_t)(ev_delete->file_ref >> 48);
        printf("             MFT#      : %" PRIu64 " (seq=%u)\n", mft, seq);

        if (moved_to_recycle) {
            printf("\n  [判定] ゴミ箱経由の削除\n");
            printf("         (FILE_DELETE はゴミ箱から「完全に削除」した時刻)\n");
        } else {
            printf("\n  [判定] 直接削除 (Shift+Delete またはプログラムによる削除)\n");
        }
    } else {
        printf("\n  [!] FILE_DELETE イベントは記録されていません。\n");
        printf("      → ジャーナルのローテーション等で上書きされた可能性があります。\n");
    }

    if (ev_close_after_delete) {
        filetime_to_str(ev_close_after_delete->timestamp, ts, sizeof(ts));
        printf("  [CLOSE]    %s\n", ts);
    }

    if (ev_last && ev_last != ev_delete) {
        filetime_to_str(ev_last->timestamp, ts, sizeof(ts));
        printf("  [最終記録] %s\n", ts);
    }

    printf("╚══════════════════════════════════════════════════════════════╝\n");

    /* イベントシーケンス */
    printf("\n# イベントシーケンス (%d件)\n", g_match_count);
    printf("%-36s  %-8s  %s\n", "タイムスタンプ", "USN下位", "操作");
    printf("%-36s  %-8s  %s\n", "------------------------------------",
           "--------", "----------------------------");
    for (int i = 0; i < g_match_count; i++) {
        MatchRecord *mr = &g_matches[i];
        filetime_to_str(mr->timestamp, ts, sizeof(ts));
        char reason_str[512];
        reason_to_str(mr->reason, reason_str, sizeof(reason_str));
        /* Shorten USN to lower 8 digits for readability */
        printf("%-36s  %08" PRIX64 "  %s\n",
               ts,
               (uint64_t)(mr->usn & 0xFFFFFFFF),
               reason_str);
    }
}

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] -j <$J_file>\n\n"
        "Options:\n"
        "  -j <file>     USN Journal ($J stream) ファイル  [必須]\n"
        "  -n <name>     ファイル名で検索 (大文字小文字無視)\n"
        "  -r <ref>      FileReferenceNumber (MFT番号) で検索 (10進数)\n"
        "  -d            FILE_DELETE イベントのみ表示\n"
        "  -c            CSV出力\n"
        "  -v            詳細表示\n"
        "  --all         全レコードを出力\n"
        "  -h            このヘルプ\n\n"
        "例:\n"
        "  %s -j usnjournal.bin -n \"important.docx\"\n"
        "  %s -j usnjournal.bin -r 12345\n"
        "  %s -j usnjournal.bin -n \"secret.xlsx\" -c > output.csv\n"
        "  %s -j usnjournal.bin -d -n \"deleted_file.pdf\"\n\n"
        "$J抽出例 (The Sleuth Kit):\n"
        "  icat -o <partition_offset_sectors> image.e01 <$UsnJrnl_MFT#>-128-1 \\\n"
        "       > usnjournal.bin\n"
        "  ※ $UsnJrnl の MFT# は: ils -a image.e01 | grep UsnJrnl\n\n",
        prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    const char *journal_file = NULL;
    const char *target_name  = NULL;
    uint64_t    target_ref   = 0;
    int         search_by_ref = 0;

    static struct option long_opts[] = {
        { "all",     no_argument, 0, 'A' },
        { "help",    no_argument, 0, 'h' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "j:n:r:dcvhA", long_opts, NULL)) != -1) {
        switch (c) {
        case 'j': journal_file   = optarg; break;
        case 'n': target_name    = optarg; break;
        case 'r':
            target_ref   = (uint64_t)strtoull(optarg, NULL, 10);
            search_by_ref = 1;
            break;
        case 'd': opt_delete_only = 1; break;
        case 'c': opt_csv        = 1; break;
        case 'v': opt_verbose    = 1; break;
        case 'A': opt_all        = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!journal_file) {
        fprintf(stderr, "[ERROR] -j オプションで $J ファイルを指定してください。\n\n");
        usage(argv[0]);
        return 1;
    }
    if (!opt_all && !target_name && !search_by_ref) {
        fprintf(stderr, "[ERROR] -n または -r で検索対象を指定してください。"
                        " (全件表示は --all)\n\n");
        usage(argv[0]);
        return 1;
    }

    FILE *fp = fopen(journal_file, "rb");
    if (!fp) {
        perror(journal_file);
        return 1;
    }

    if (!opt_csv) {
        printf("=========================================================\n");
        printf(" USN Journal Analyzer - 削除日時精密特定ツール\n");
        printf("=========================================================\n");
        printf(" ジャーナルファイル: %s\n", journal_file);
        if (target_name)
            printf(" 検索ファイル名   : %s\n", target_name);
        if (search_by_ref)
            printf(" MFT番号          : %" PRIu64 "\n", target_ref);
        printf("=========================================================\n\n");
    }

    parse_usn_journal(fp, target_name, target_ref, search_by_ref);
    fclose(fp);

    print_summary();

    free(g_matches);
    return 0;
}
