/*
 * usn_carve.c - USN レコードのカービング（署名ベース復元）ツール
 *
 * $UsnJrnl:$J のように「レコードが隙間なく並んでいる」ことを前提にせず、
 * 任意の生データ（区画イメージ全体・未割当領域・pagefile.sys・hiberfil.sys
 * ・$LogFile・メモリダンプなど）を1バイトずつ走査し、USN_RECORD_V2 の
 * 署名に合致する箇所を拾ってCSVに出力する。
 *
 * 狙い:
 *   USNジャーナルはサイズ上限に達すると割り当て差分の分だけ古いクラスタを
 *   解放する。解放されただけで中身は消えないため、再利用されるまで過去の
 *   USNレコードがディスク上に残る。`fsutil usn deletejournal` による消去
 *   （NotPetya 等）も同じで、クラスタは解放されるだけ。これらを拾い直す。
 *
 * 署名（正常な USN_RECORD_V2 の性質から決める。ノイズを減らすのが肝）:
 *   - RecordLength : 8 の倍数、かつ 0x3C+2 〜 0x400 の範囲
 *   - MajorVersion : 2 / MinorVersion : 0
 *   - FileNameOffset: 0x3C（V2 の固定値）
 *   - FileNameLength: 偶数、2〜510、かつ 0x3C+長さ <= RecordLength
 *   - TimeStamp    : 妥当な FileTime（西暦 1990〜2100 に収まる）
 *
 * Build:
 *   cc -O2 -Wall -o usn_carve usn_carve.c   # Linux / macOS 共通
 *
 * Usage:
 *   sudo ./usn_carve /dev/loop0p1 > carved.csv   # 区画全体を走査
 *   ./usn_carve pagefile.sys       > carved.csv
 *   ./usn_carve unalloc.bin        | grep FILE_DELETE
 *
 * CSV列（先頭8列は usn_decode と同じ。末尾に発見オフセットを足す）:
 *   MFT_Num,MFT_Seq,Parent_MFT_Num,USN,Timestamp,Reason,FileAttr,Filename,SrcOffset
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#define MAX_INPUT  (2ULL * 1024 * 1024 * 1024)  /* 2 GB 上限（超える場合は分割） */
#define REC_MIN    0x3Eu    /* 0x3C ヘッダ + 最低 2 バイトのファイル名 */
#define REC_MAX    0x400u   /* 署名の上限（0x3C + 510B 名 + 余裕） */

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

/* FILETIME → "YYYY-MM-DD HH:MM:SS.ffffff"、年を out_year に返す */
static void ft_to_str(int64_t ft, char *buf, size_t len, int *out_year) {
    *out_year = -1;
    if (ft <= 0) { buf[0] = '\0'; return; }
    uint64_t uft    = (uint64_t)ft;
    uint32_t sub_us = (uint32_t)((uft % 10000000ULL) / 10);
    uint64_t unix_s = uft / 10000000ULL - 11644473600ULL;
    time_t t = (time_t)unix_s;
    struct tm *tm = gmtime(&t);
    if (!tm) { snprintf(buf, len, "INVALID"); return; }
    *out_year = tm->tm_year + 1900;
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

/* UTF-16LE → UTF-8 (BMP only)。印字不能や非文字が混ざれば 0 を返す（ノイズ除け） */
static int u16_to_u8(const uint8_t *s, int sb, char *d, int dl) {
    int di = 0;
    for (int i = 0; i+1 < sb && di < dl-4; i += 2) {
        uint16_t w = s[i] | (s[i+1]<<8);
        if (!w) break;
        if (w < 0x20) return 0;                 /* 制御文字 = USN 名らしくない */
        if (w < 0x80)        { d[di++] = w; }
        else if (w < 0x800)  { d[di++]=0xC0|(w>>6); d[di++]=0x80|(w&0x3F); }
        else { d[di++]=0xE0|(w>>12); d[di++]=0x80|((w>>6)&0x3F); d[di++]=0x80|(w&0x3F); }
    }
    d[di] = '\0';
    return di > 0;
}

/* オフセット p に妥当な USN_RECORD_V2 の署名があるか。あれば fname/ts/rs を埋める */
static int match_record(const uint8_t *b, uint64_t p, uint64_t size,
                        char *fname, size_t fnl, char *ts, size_t tsl,
                        char *rs, size_t rsl, const USN_RECORD_V2 **out) {
    if (p + sizeof(USN_RECORD_V2) > size) return 0;
    const USN_RECORD_V2 *r = (const USN_RECORD_V2 *)(b + p);

    /* 固定部分の署名 */
    if (r->MajorVersion != 2 || r->MinorVersion != 0)          return 0;
    if (r->FileNameOffset != 0x3C)                             return 0;
    if ((r->RecordLength & 7) != 0)                            return 0;
    if (r->RecordLength < REC_MIN || r->RecordLength > REC_MAX) return 0;
    if (p + r->RecordLength > size)                            return 0;
    if ((r->FileNameLength & 1) != 0 ||
        r->FileNameLength < 2 || r->FileNameLength > 510)      return 0;
    if ((uint32_t)r->FileNameOffset + r->FileNameLength > r->RecordLength) return 0;

    /* タイムスタンプが妥当な FileTime か（ランダムデータ除け） */
    int year;
    ft_to_str(r->TimeStamp, ts, tsl, &year);
    if (year < 1990 || year > 2100) return 0;

    /* ファイル名が印字可能な文字列として解釈できるか */
    if (!u16_to_u8((const uint8_t *)r + r->FileNameOffset,
                   r->FileNameLength, fname, (int)fnl)) return 0;

    reason_str(r->Reason, rs, rsl);
    *out = r;
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "使い方: %s <入力ファイル/デバイス> [> carved.csv]\n"
            "  任意の生データを走査して USN レコードをカービングする。\n"
            "  例: sudo %s /dev/loop0p1 > carved.csv   # 区画全体\n"
            "      %s pagefile.sys | grep FILE_DELETE\n\n"
            "CSV 列: MFT_Num,MFT_Seq,Parent_MFT_Num,USN,Timestamp,"
            "Reason,FileAttr,Filename,SrcOffset\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 1; }

    /* 入力全体をメモリに読み込む（カービングは全域走査が前提） */
    fseek(fp, 0, SEEK_END);
    long long sz = ftello(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "Error: 空または読み取り失敗\n"); fclose(fp); return 1; }
    if ((unsigned long long)sz > MAX_INPUT) {
        fprintf(stderr, "Error: 入力が大きすぎます（%lld MB > %llu MB）。"
                "dd で分割して走査してください。\n",
                sz/1024/1024, MAX_INPUT/1024/1024);
        fclose(fp); return 1;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { perror("malloc"); fclose(fp); return 1; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fprintf(stderr, "Error: 読み取り不完全\n"); free(buf); fclose(fp); return 1;
    }
    fclose(fp);
    fprintf(stderr, "# 入力サイズ: %lld bytes を走査\n", sz);

    printf("MFT_Num,MFT_Seq,Parent_MFT_Num,USN,Timestamp,Reason,FileAttr,Filename,SrcOffset\n");

    uint64_t size = (uint64_t)sz;
    uint64_t p = 0, found = 0;
    char fname[768], ts[32], rs[512];
    const USN_RECORD_V2 *r;

    while (p + sizeof(USN_RECORD_V2) <= size) {
        if (match_record(buf, p, size, fname, sizeof(fname),
                         ts, sizeof(ts), rs, sizeof(rs), &r)) {
            uint64_t mft_num = r->FileReferenceNumber & 0x0000FFFFFFFFFFFFULL;
            uint16_t mft_seq = (uint16_t)(r->FileReferenceNumber >> 48);
            uint64_t par_mft = r->ParentFileReferenceNumber & 0x0000FFFFFFFFFFFFULL;
            int need_q = (strchr(fname, ',') || strchr(fname, '"'));
            printf("%" PRIu64 ",%u,%" PRIu64 ",%" PRId64 ",%s,%s,0x%08X,",
                   mft_num, mft_seq, par_mft, (long long)r->Usn, ts, rs,
                   r->FileAttributes);
            if (need_q) printf("\"%s\",0x%" PRIx64 "\n", fname, p);
            else        printf("%s,0x%" PRIx64 "\n", fname, p);
            found++;
            /* 連続した正規ジャーナル部分は RecordLength で辿る */
            p += r->RecordLength;
        } else {
            p += 8;   /* USN レコードは 8 バイト境界。8 刻みで次を探す */
        }
    }

    free(buf);
    fprintf(stderr, "# カービングで復元した USN レコード数: %" PRIu64 "\n", found);
    return 0;
}
