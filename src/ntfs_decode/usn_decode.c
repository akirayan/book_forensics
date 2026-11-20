/*
 * usn_decode.c - USN Journal ($UsnJrnl:$J) → CSV ダンプツール
 *
 * 全レコードをCSVに出力するだけ。分析はgrep/awk/Excelで。
 *
 * Build:
 *   gcc -o usn_decode usn_decode.c -Wall -O2
 *
 * Usage:
 *   ./usn_decode usnjournal.bin > usn.csv
 *   ./usn_decode usnjournal.bin | grep -i "important.docx"
 *   ./usn_decode usnjournal.bin | grep "FILE_DELETE"
 *   ./usn_decode usnjournal.bin | grep "^12345,"
 *
 * $J抽出 (The Sleuth Kit):
 *   fls image.e01  | grep -i extend
 *   icat -o <offset> image.e01 <$UsnJrnl MFT#>-128-1 > usnjournal.bin
 *
 * CSV列:
 *   MFT_Num, MFT_Seq, Parent_MFT_Num, USN, Timestamp, Reason, FileAttr, Filename
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t RecordLength;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint64_t FileReferenceNumber;
    uint64_t ParentFileReferenceNumber;
    int64_t  Usn;
    int64_t  TimeStamp;
    uint32_t Reason;
    uint32_t SourceInfo;
    uint32_t SecurityId;
    uint32_t FileAttributes;
    uint16_t FileNameLength;
    uint16_t FileNameOffset;
} USN_RECORD_V2;
#pragma pack(pop)

/* FILETIME → "YYYY-MM-DD HH:MM:SS.ffffff" */
static void ft_to_str(int64_t ft, char *buf, size_t len) {
    if (ft <= 0) { snprintf(buf, len, ""); return; }
    uint64_t uft     = (uint64_t)ft;
    uint32_t sub_us  = (uint32_t)((uft % 10000000ULL) / 10);
    uint64_t unix_s  = uft / 10000000ULL - 11644473600ULL;
    time_t t = (time_t)unix_s;
    struct tm *tm = gmtime(&t);
    if (!tm) { snprintf(buf, len, "INVALID"); return; }
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d.%06u",
             tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, sub_us);
}

/* Reason flags → 文字列 */
static void reason_str(uint32_t r, char *buf, size_t len) {
    static const struct { uint32_t f; const char *n; } T[] = {
        {0x00000001,"DATA_OVERWRITE"},   {0x00000002,"DATA_EXTEND"},
        {0x00000004,"DATA_TRUNCATION"},  {0x00000010,"NAMED_DATA_OVERWRITE"},
        {0x00000020,"NAMED_DATA_EXTEND"},{0x00000040,"NAMED_DATA_TRUNCATION"},
        {0x00000100,"FILE_CREATE"},      {0x00000200,"FILE_DELETE"},
        {0x00000400,"EA_CHANGE"},        {0x00000800,"SECURITY_CHANGE"},
        {0x00001000,"RENAME_OLD_NAME"},  {0x00002000,"RENAME_NEW_NAME"},
        {0x00004000,"INDEXABLE_CHANGE"}, {0x00008000,"BASIC_INFO_CHANGE"},
        {0x00010000,"HARD_LINK_CHANGE"}, {0x00020000,"COMPRESSION_CHANGE"},
        {0x00040000,"ENCRYPTION_CHANGE"},{0x00080000,"OBJECT_ID_CHANGE"},
        {0x00100000,"REPARSE_POINT_CHANGE"},{0x00200000,"STREAM_CHANGE"},
        {0x80000000,"CLOSE"},            {0,NULL}
    };
    size_t off = 0;
    buf[0] = '\0';
    for (int i = 0; T[i].n; i++) {
        if (!(r & T[i].f)) continue;
        if (off > 0 && off < len-2) buf[off++] = '|';
        size_t nl = strlen(T[i].n);
        if (off + nl < len-1) { memcpy(buf+off, T[i].n, nl); off += nl; buf[off]='\0'; }
    }
    if (!off) snprintf(buf, len, "0x%08X", r);
}

/* UTF-16LE → UTF-8 (BMP only) */
static void u16_to_u8(const uint8_t *s, int sb, char *d, int dl) {
    int di = 0;
    for (int i = 0; i+1 < sb && di < dl-4; i += 2) {
        uint16_t w = s[i] | (s[i+1]<<8);
        if (!w) break;
        if (w < 0x80)        { d[di++] = w; }
        else if (w < 0x800)  { d[di++]=0xC0|(w>>6); d[di++]=0x80|(w&0x3F); }
        else { d[di++]=0xE0|(w>>12); d[di++]=0x80|((w>>6)&0x3F); d[di++]=0x80|(w&0x3F); }
    }
    d[di] = '\0';
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <$J_file> [> output.csv]\n", argv[0]);
        fprintf(stderr, "\nCSV columns:\n");
        fprintf(stderr, "  MFT_Num,MFT_Seq,Parent_MFT_Num,USN,Timestamp,Reason,FileAttr,Filename\n");
        fprintf(stderr, "\nUseful grep patterns:\n");
        fprintf(stderr, "  grep -i 'target.docx'   # ファイル名で絞り込み\n");
        fprintf(stderr, "  grep 'FILE_DELETE'       # 削除イベントのみ\n");
        fprintf(stderr, "  grep '^12345,'           # MFT番号で絞り込み\n");
        return 1;
    }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 1; }

    /* CSV header */
    printf("MFT_Num,MFT_Seq,Parent_MFT_Num,USN,Timestamp,Reason,FileAttr,Filename\n");

    uint8_t  buf[65536];
    uint64_t file_offset = 0;
    size_t   buf_used = 0;
    size_t   bytes_read;

    while ((bytes_read = fread(buf + buf_used, 1,
                               sizeof(buf) - buf_used, fp)) > 0 || buf_used > 0) {
        if (!bytes_read && !buf_used) break;
        if (!bytes_read) break;  /* EOF: 残りバイトは不完全レコード → 終了 */
        buf_used += bytes_read;
        size_t pos = 0;

        while (pos + sizeof(USN_RECORD_V2) <= buf_used) {

            /* スパース(ゼロ埋め)スキップ */
            if (*(uint32_t *)(buf+pos) == 0) {
                size_t skip = 0;
                while (pos+skip+3 < buf_used && *(uint32_t *)(buf+pos+skip) == 0)
                    skip += 8;
                file_offset += skip;
                pos += skip;
                continue;
            }

            USN_RECORD_V2 *rec = (USN_RECORD_V2 *)(buf+pos);

            if (rec->RecordLength < sizeof(USN_RECORD_V2) ||
                rec->RecordLength > 65536 ||
                (rec->RecordLength & 7) != 0 ||
                rec->MajorVersion != 2) {
                file_offset += 8; pos += 8; continue;
            }
            if (pos + rec->RecordLength > buf_used) break;

            /* ファイル名 */
            char fname[512] = "";
            if (rec->FileNameOffset + rec->FileNameLength <= rec->RecordLength)
                u16_to_u8((uint8_t*)rec + rec->FileNameOffset,
                          rec->FileNameLength, fname, sizeof(fname));

            /* タイムスタンプ */
            char ts[32];
            ft_to_str(rec->TimeStamp, ts, sizeof(ts));

            /* Reason */
            char rs[512];
            reason_str(rec->Reason, rs, sizeof(rs));

            /* MFT番号 */
            uint64_t mft_num = rec->FileReferenceNumber & 0x0000FFFFFFFFFFFFULL;
            uint16_t mft_seq = (uint16_t)(rec->FileReferenceNumber >> 48);
            uint64_t par_mft = rec->ParentFileReferenceNumber & 0x0000FFFFFFFFFFFFULL;

            /* CSV出力 (ファイル名にカンマがあればクォート) */
            int need_q = (strchr(fname, ',') || strchr(fname, '"'));
            printf("%" PRIu64 ",%u,%" PRIu64 ",%" PRId64 ",%s,%s,0x%08X,",
                   mft_num, mft_seq, par_mft, rec->Usn, ts, rs, rec->FileAttributes);
            if (need_q) printf("\"%s\"\n", fname);
            else        printf("%s\n", fname);

            pos         += rec->RecordLength;
        }

        if (pos > 0 && pos < buf_used) {
            memmove(buf, buf+pos, buf_used-pos);
            file_offset += pos;
            buf_used    -= pos;
        } else if (pos >= buf_used) {
            file_offset += pos;
            buf_used     = 0;
        }
    }

    fclose(fp);
    return 0;
}
