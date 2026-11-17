/*
 * mft_tm_decode.c - NTFS MFT タイムスタンプ解析ツール
 *
 * 各 MFT レコードから $STANDARD_INFORMATION と $FILE_NAME の
 * 8 つのタイムスタンプをすべて抽出し CSV に出力する【デコード専用】。
 *
 * Timestomping の判定は本ツールでは行わない。出力した CSV を別ツール
 * tm_judge に渡してスコアリングする（Unix 流に「取り出す」と「判断する」を分離）:
 *   sudo ./mft_tm_decode /dev/loop0p1 | ./tm_judge
 *
 * 出力列:
 *   mft_ref, status,
 *   si_created, si_modified, si_mft_modified, si_accessed,
 *   fn_created, fn_modified, fn_mft_modified, fn_accessed,
 *   filename
 *
 * コンパイル:
 *   cc -O2 -Wall -o mft_tm_decode mft_tm_decode.c            # Linux
 *   cc -O2 -Wall -o mft_tm_decode mft_tm_decode.c -liconv    # macOS（iconv を明示リンク）
 *
 * 実行:
 *   sudo ./mft_tm_decode /dev/loop0p1
 *   sudo ./mft_tm_decode /dev/loop0p1 timestamps.csv
 *
 * 注意:
 *   本ツールはタイムスタンプに特化した軽量ツールであり、
 *   フルパス解決は行わない（MFT# とファイル名のみ出力）。
 *   フルパスが必要な場合は mft_decode を使用すること。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <iconv.h>
#include <locale.h>

/* ============================================================
 *  定数
 * ============================================================ */
#define MFT_RECORD_MAGIC           0x454C4946U   /* "FILE" */
#define MFT_RECORD_IN_USE          0x0001
#define MFT_RECORD_IS_DIR          0x0002

#define ATTR_STANDARD_INFORMATION  0x10U
#define ATTR_FILE_NAME             0x30U
#define ATTR_END                   0xFFFFFFFFU

#define FILE_NAME_POSIX            0
#define FILE_NAME_WIN32            1
#define FILE_NAME_DOS              2
#define FILE_NAME_WIN32DOS         3

#define MAX_FN_ENTRIES             16
#define MAX_NAME_UTF8              768   /* 255 UTF-16 文字 → 最大 765 UTF-8 バイト */

/* ============================================================
 *  オンディスク構造体
 * ============================================================ */
#pragma pack(push, 1)

typedef struct {
    uint8_t  jump[3];
    uint8_t  oem_id[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint8_t  reserved1[7];
    uint8_t  media_type;
    uint16_t reserved2;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t reserved3;
    uint32_t reserved4;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mft_mirror_lcn;
    int8_t   clusters_per_mft_record;
    uint8_t  reserved5[3];
    int8_t   clusters_per_index_block;
    uint8_t  reserved6[3];
    uint64_t volume_serial;
    uint32_t checksum;
} NTFS_BOOT;

typedef struct {
    uint32_t magic;
    uint16_t usa_offset;
    uint16_t usa_count;
    uint64_t log_seq;
    uint16_t seq_number;
    uint16_t hard_links;
    uint16_t attrs_offset;
    uint16_t flags;
    uint32_t bytes_used;
    uint32_t bytes_alloc;
    uint64_t base_mft_ref;
    uint16_t next_attr_id;
    uint16_t padding;
    uint32_t mft_record_number;
} MFT_RECORD;

typedef struct {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attr_id;
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t  indexed;
    uint8_t  pad;
} ATTR_RES;

typedef struct {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attr_id;
    uint64_t start_vcn;
    uint64_t end_vcn;
    uint16_t runlist_offset;
    uint16_t compression_unit;
    uint32_t pad;
    uint64_t alloc_size;
    uint64_t data_size;
    uint64_t init_size;
} ATTR_NONRES;

/* $STANDARD_INFORMATION（最低 48 バイト） */
typedef struct {
    uint64_t created;
    uint64_t modified;
    uint64_t mft_modified;
    uint64_t accessed;
    /* 続きのフィールドは本ツールでは使用しない */
} STD_INFO;

/* $FILE_NAME */
typedef struct {
    uint64_t parent_ref;
    uint64_t created;
    uint64_t modified;
    uint64_t mft_modified;
    uint64_t accessed;
    uint64_t alloc_size;
    uint64_t data_size;
    uint32_t file_attrs;
    uint32_t reparse_tag;
    uint8_t  name_length;
    uint8_t  name_namespace;
    /* 続いて name_length * 2 バイトの UTF-16LE 文字列 */
} FILE_NAME_ATTR;

#pragma pack(pop)

/* ============================================================
 *  ランリストエントリ（MFT#0 のデータ位置解決に使用）
 * ============================================================ */
typedef struct {
    uint64_t lcn;      /* LCN（(uint64_t)-1 = スパース） */
    uint64_t run_len;  /* クラスタ数 */
} RunEntry;

/* ============================================================
 *  グローバル（ボリューム情報）
 * ============================================================ */
static int      g_dev_fd          = -1;
static uint32_t g_bytes_per_sec   = 512;
static uint32_t g_bytes_per_clus  = 4096;
static uint32_t g_mft_record_size = 1024;
static uint64_t g_mft_byte_offset = 0;

/* ============================================================
 *  ユーティリティ関数
 * ============================================================ */

/*
 * pread の分割読み込みラッパー。
 * 短い読み込みが発生した場合でも count バイト読むまでリトライする。
 */
static ssize_t full_pread(int fd, void *buf, size_t count, off_t offset) {
    size_t done = 0;
    while (done < count) {
        ssize_t r = pread(fd, (char *)buf + done, count - done,
                          offset + (off_t)done);
        if (r <= 0) return (done > 0) ? (ssize_t)done : r;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/*
 * NTFS の FileTime（100ns 単位、基点 1601-01-01 UTC）を
 * "YYYY-MM-DD HH:MM:SS.XXXXXXX" 形式の文字列に変換する。
 *
 * 末尾の 7 桁が 100ns 単位のサブ秒部分（= FileTime % 10000000）。
 * ts == 0 の場合は空文字列を返す。
 */
static void filetime_to_str(uint64_t ts, char *buf, size_t buflen) {
    if (ts == 0) {
        if (buflen > 0) buf[0] = '\0';
        return;
    }
    /* 100ns 以下のサブ秒（0〜9999999） */
    uint32_t sub7 = (uint32_t)(ts % 10000000ULL);
    /* Unix エポックへの変換: 1601-01-01 → 1970-01-01 の差 = 116444736000000000 × 100ns */
    time_t t = (time_t)(ts / 10000000ULL) - 11644473600ULL;
    struct tm *tm_utc = gmtime(&t);
    if (!tm_utc) {
        if (buflen > 0) buf[0] = '\0';
        return;
    }
    char base[32];
    strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", tm_utc);
    snprintf(buf, buflen, "%s.%07u", base, sub7);
}

/*
 * UTF-16LE バイト列を UTF-8 文字列に変換する（iconv 使用）。
 */
static void utf16le_to_utf8(const uint8_t *src, size_t src_bytes,
                             char *dst, size_t dst_bytes) {
    if (src_bytes == 0 || dst_bytes == 0) {
        if (dst_bytes > 0) dst[0] = '\0';
        return;
    }
    iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
    if (cd == (iconv_t)-1) { dst[0] = '\0'; return; }
    char   *in   = (char *)src;
    char   *out  = dst;
    size_t  inl  = src_bytes;
    size_t  outl = dst_bytes - 1;
    iconv(cd, &in, &inl, &out, &outl);
    *out = '\0';
    iconv_close(cd);
}

/*
 * CSV フィールドを RFC 4180 準拠でダブルクォート囲みで出力する。
 * フィールド内のダブルクォートは "" にエスケープする。
 */
static void csv_field(FILE *fp, const char *s) {
    fputc('"', fp);
    for (; *s; s++) {
        if (*s == '"') fputc('"', fp);
        fputc(*s, fp);
    }
    fputc('"', fp);
}

/* ============================================================
 *  Fix-up（USA 適用）
 * ============================================================ */
static void apply_fixup(uint8_t *rec, uint32_t rec_size) {
    MFT_RECORD *mft = (MFT_RECORD *)rec;
    if (mft->usa_offset == 0 || mft->usa_count < 2) return;
    uint16_t *usa = (uint16_t *)(rec + mft->usa_offset);
    uint32_t  n   = mft->usa_count - 1; /* セクタ数 */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t  off = (i + 1) * g_bytes_per_sec - 2;
        if (off + 2 > rec_size) break;
        uint16_t *p = (uint16_t *)(rec + off);
        *p = usa[i + 1]; /* 元の値に復元 */
    }
}

/* ============================================================
 *  ランリスト解析
 * ============================================================ */
static int parse_runlist(const uint8_t *rl, size_t max_len,
                         RunEntry *runs, int max_runs) {
    int     n       = 0;
    size_t  pos     = 0;
    int64_t cur_lcn = 0;

    while (pos < max_len && n < max_runs) {
        uint8_t hdr = rl[pos++];
        if (hdr == 0) break;

        int lbytes = hdr & 0x0F;
        int obytes = (hdr >> 4) & 0x0F;
        if (pos + (size_t)(lbytes + obytes) > max_len) break;

        /* run_length（符号なし） */
        uint64_t rlen = 0;
        for (int i = 0; i < lbytes; i++)
            rlen |= (uint64_t)rl[pos++] << (i * 8);

        /* lcn_delta（符号付き、符号拡張が必要） */
        int64_t delta = 0;
        for (int i = 0; i < obytes; i++)
            delta |= (int64_t)rl[pos++] << (i * 8);
        if (obytes > 0 && (rl[pos - 1] & 0x80))
            for (int i = obytes; i < 8; i++)
                delta |= (int64_t)0xFF << (i * 8);

        if (obytes == 0) {
            runs[n].lcn = (uint64_t)-1; /* スパース */
        } else {
            cur_lcn += delta;
            runs[n].lcn = (uint64_t)cur_lcn;
        }
        runs[n].run_len = rlen;
        n++;
    }
    return n;
}

/* ============================================================
 *  タイムスタンプセット（$SI または $FN の 4 つ）
 * ============================================================ */
typedef struct {
    uint64_t created;
    uint64_t modified;
    uint64_t mft_modified;
    uint64_t accessed;
    int      valid; /* 1=属性が存在した, 0=存在しない */
} TsSet;

/* 判定（Timestomping スコアリング）は本ツールでは行わない。
 * 出力した 8 タイムスタンプ CSV を、別ツール tm_judge に渡して評価する
 * （Unix 流に「取り出す」と「判断する」を分離）。 */

/* ============================================================
 *  レコード 1 件の処理
 * ============================================================ */
static void process_record(const uint8_t *rec_buf, FILE *out_fp,
                            uint64_t *cnt_emitted) {
    const MFT_RECORD *mft = (const MFT_RECORD *)rec_buf;

    const char *status = (mft->flags & MFT_RECORD_IN_USE) ? "IN_USE" : "DELETED";

    TsSet si = {0};
    TsSet fn = {0};

    /* $FILE_NAME は複数存在する場合があるため Win32 優先で選択 */
    typedef struct {
        TsSet    ts;
        char     name[MAX_NAME_UTF8];
        uint8_t  ns;
    } FnEntry;
    FnEntry fn_all[MAX_FN_ENTRIES];
    int fn_count = 0;

    /* ── 属性ウォーク ── */
    uint32_t off = mft->attrs_offset;
    while (off + 8 <= g_mft_record_size) {
        uint32_t atype = *(uint32_t *)(rec_buf + off);
        uint32_t alen  = *(uint32_t *)(rec_buf + off + 4);
        if (atype == ATTR_END || alen == 0) break;
        if (off + alen > g_mft_record_size) break;

        const ATTR_RES *ar = (const ATTR_RES *)(rec_buf + off);

        if (!ar->non_resident) {
            /* 常駐属性のみ（$SI と $FN は必ず常駐） */
            if (ar->value_offset + ar->value_length > alen) goto next_attr;
            const uint8_t *val = rec_buf + off + ar->value_offset;

            if (atype == ATTR_STANDARD_INFORMATION &&
                ar->value_length >= (uint32_t)sizeof(STD_INFO)) {
                const STD_INFO *s = (const STD_INFO *)val;
                si.created      = s->created;
                si.modified     = s->modified;
                si.mft_modified = s->mft_modified;
                si.accessed     = s->accessed;
                si.valid        = 1;
            }

            if (atype == ATTR_FILE_NAME &&
                ar->value_length >= (uint32_t)sizeof(FILE_NAME_ATTR) &&
                fn_count < MAX_FN_ENTRIES) {
                const FILE_NAME_ATTR *fna = (const FILE_NAME_ATTR *)val;
                uint32_t nbytes = fna->name_length * 2u;
                if (sizeof(FILE_NAME_ATTR) + nbytes <= ar->value_length) {
                    FnEntry *fe = &fn_all[fn_count];
                    fe->ts.created      = fna->created;
                    fe->ts.modified     = fna->modified;
                    fe->ts.mft_modified = fna->mft_modified;
                    fe->ts.accessed     = fna->accessed;
                    fe->ts.valid        = 1;
                    fe->ns = fna->name_namespace;
                    utf16le_to_utf8(val + sizeof(FILE_NAME_ATTR), nbytes,
                                    fe->name, MAX_NAME_UTF8);
                    fn_count++;
                }
            }
        }
    next_attr:
        off += alen;
    }

    /* $SI が存在しない場合はスキップ（拡張レコード等） */
    if (!si.valid) return;

    /* Win32 > Win32&DOS > POSIX > DOS の優先順で最適 $FN を選択 */
    char best_name[MAX_NAME_UTF8] = "";
    int  best_idx = -1;

    for (int i = 0; i < fn_count; i++) {
        if (fn_all[i].ns == FILE_NAME_WIN32 ||
            fn_all[i].ns == FILE_NAME_WIN32DOS) {
            best_idx = i;
            break;
        }
    }
    if (best_idx < 0) {
        /* Win32 が見つからなければ POSIX → DOS の順 */
        for (int i = 0; i < fn_count; i++) {
            if (fn_all[i].ns == FILE_NAME_POSIX) { best_idx = i; break; }
        }
    }
    if (best_idx < 0 && fn_count > 0) best_idx = 0;

    if (best_idx >= 0) {
        fn = fn_all[best_idx].ts;
        snprintf(best_name, MAX_NAME_UTF8, "%s", fn_all[best_idx].name);
    }

    /* ── CSV 出力（デコードのみ。判定は別ツール tm_judge が行う） ── */
    char si_cr[40], si_mo[40], si_mm[40], si_ac[40];
    char fn_cr[40], fn_mo[40], fn_mm[40], fn_ac[40];
    filetime_to_str(si.created,      si_cr, sizeof(si_cr));
    filetime_to_str(si.modified,     si_mo, sizeof(si_mo));
    filetime_to_str(si.mft_modified, si_mm, sizeof(si_mm));
    filetime_to_str(si.accessed,     si_ac, sizeof(si_ac));
    filetime_to_str(fn.created,      fn_cr, sizeof(fn_cr));
    filetime_to_str(fn.modified,     fn_mo, sizeof(fn_mo));
    filetime_to_str(fn.mft_modified, fn_mm, sizeof(fn_mm));
    filetime_to_str(fn.accessed,     fn_ac, sizeof(fn_ac));

    /*
     * mft_ref,status,
     * si_created,si_modified,si_mft_modified,si_accessed,
     * fn_created,fn_modified,fn_mft_modified,fn_accessed,
     * filename
     */
    fprintf(out_fp, "%llu:%u,%s,",
            (unsigned long long)mft->mft_record_number,
            (unsigned)mft->seq_number,
            status);
    csv_field(out_fp, si_cr); fputc(',', out_fp);
    csv_field(out_fp, si_mo); fputc(',', out_fp);
    csv_field(out_fp, si_mm); fputc(',', out_fp);
    csv_field(out_fp, si_ac); fputc(',', out_fp);
    csv_field(out_fp, fn_cr); fputc(',', out_fp);
    csv_field(out_fp, fn_mo); fputc(',', out_fp);
    csv_field(out_fp, fn_mm); fputc(',', out_fp);
    csv_field(out_fp, fn_ac); fputc(',', out_fp);
    csv_field(out_fp, best_name); fputc('\n', out_fp);

    (*cnt_emitted)++;
}

/* ============================================================
 *  MFT 全走査
 * ============================================================ */
static void scan_mft(const uint8_t *mft0_buf, FILE *out_fp,
                     uint64_t *cnt_emitted,
                     uint64_t *cnt_total) {
    const MFT_RECORD *mft0 = (const MFT_RECORD *)mft0_buf;

    /* MFT#0 の $DATA ランリスト取得 */
    uint64_t        data_size       = 0;
    const uint8_t  *runlist_ptr     = NULL;
    uint32_t        runlist_max_len = 0;

    uint32_t off = mft0->attrs_offset;
    while (off + 8 <= g_mft_record_size) {
        uint32_t atype = *(uint32_t *)(mft0_buf + off);
        uint32_t alen  = *(uint32_t *)(mft0_buf + off + 4);
        if (atype == ATTR_END || alen == 0) break;
        if (off + alen > g_mft_record_size) break;

        if (atype == 0x80U /* $DATA */) {
            const ATTR_NONRES *anr = (const ATTR_NONRES *)(mft0_buf + off);
            if (anr->non_resident && anr->name_length == 0) {
                data_size       = anr->data_size;
                runlist_ptr     = mft0_buf + off + anr->runlist_offset;
                runlist_max_len = alen - anr->runlist_offset;
                break;
            }
        }
        off += alen;
    }

    if (!runlist_ptr) {
        fprintf(stderr, "[エラー] MFT#0 の $DATA ランリストが見つかりません\n");
        return;
    }

    RunEntry runs[4096];
    int num_runs = parse_runlist(runlist_ptr, runlist_max_len, runs, 4096);
    if (num_runs == 0) {
        fprintf(stderr, "[エラー] ランリストが空です\n");
        return;
    }

    uint64_t total_records = data_size / g_mft_record_size;
    if (cnt_total) *cnt_total = total_records;

    uint8_t *rec_buf = malloc(g_mft_record_size);
    if (!rec_buf) { perror("malloc"); return; }

    uint64_t rec_idx = 0;

    for (int ri = 0; ri < num_runs && rec_idx < total_records; ri++) {
        if (runs[ri].lcn == (uint64_t)-1) {
            /* スパースラン：レコードインデックスだけ進める */
            rec_idx += runs[ri].run_len * (g_bytes_per_clus / g_mft_record_size);
            continue;
        }

        uint64_t run_byte_off = runs[ri].lcn * g_bytes_per_clus;
        uint64_t run_byte_len = runs[ri].run_len * g_bytes_per_clus;
        uint64_t pos = 0;

        while (pos + g_mft_record_size <= run_byte_len &&
               rec_idx < total_records) {
            off_t   phys = (off_t)(run_byte_off + pos);
            ssize_t r    = full_pread(g_dev_fd, rec_buf, g_mft_record_size, phys);
            if (r == (ssize_t)g_mft_record_size) {
                MFT_RECORD *mft = (MFT_RECORD *)rec_buf;
                if (mft->magic == MFT_RECORD_MAGIC) {
                    apply_fixup(rec_buf, g_mft_record_size);
                    process_record(rec_buf, out_fp, cnt_emitted);
                }
            }
            pos     += g_mft_record_size;
            rec_idx++;
        }
    }
    free(rec_buf);
}

/* ============================================================
 *  main
 * ============================================================ */
int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    if (argc < 2) {
        fprintf(stderr,
            "使い方: %s <デバイス> [出力CSV]\n"
            "  例:  sudo %s /dev/loop0 timestamps.csv\n"
            "       sudo %s /dev/loop0\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *device   = argv[1];
    const char *csv_path = (argc >= 3) ? argv[2] : NULL;

    /* デバイスオープン */
    g_dev_fd = open(device, O_RDONLY);
    if (g_dev_fd < 0) {
        fprintf(stderr, "Error: '%s' を開けません: %s\n",
                device, strerror(errno));
        return 1;
    }

    /* ブートセクタ読み込み */
    NTFS_BOOT boot;
    if (full_pread(g_dev_fd, &boot, sizeof(boot), 0) != sizeof(boot)) {
        fprintf(stderr, "Error: ブートセクタ読み取り失敗\n");
        close(g_dev_fd); return 1;
    }
    if (memcmp(boot.oem_id, "NTFS    ", 8) != 0) {
        fprintf(stderr, "Error: NTFS シグネチャが見つかりません (OEM: %.8s)\n",
                (char *)boot.oem_id);
        close(g_dev_fd); return 1;
    }

    /* ボリュームパラメータ確定 */
    g_bytes_per_sec  = boot.bytes_per_sector;
    g_bytes_per_clus = g_bytes_per_sec * boot.sectors_per_cluster;

    if (boot.clusters_per_mft_record < 0)
        g_mft_record_size = 1u << (uint8_t)(-boot.clusters_per_mft_record);
    else if (boot.clusters_per_mft_record > 0)
        g_mft_record_size = (uint32_t)boot.clusters_per_mft_record * g_bytes_per_clus;
    else
        g_mft_record_size = 1024;

    g_mft_byte_offset = boot.mft_lcn * g_bytes_per_clus;

    fprintf(stderr, "# デバイス          : %s\n",  device);
    fprintf(stderr, "# セクタサイズ      : %u B\n", g_bytes_per_sec);
    fprintf(stderr, "# クラスタサイズ    : %u B\n", g_bytes_per_clus);
    fprintf(stderr, "# MFT レコードサイズ: %u B\n", g_mft_record_size);
    fprintf(stderr, "# MFT 物理オフセット: 0x%llx\n",
            (unsigned long long)g_mft_byte_offset);

    /* MFT#0 読み込み */
    uint8_t *mft0_buf = malloc(g_mft_record_size);
    if (!mft0_buf) { perror("malloc"); close(g_dev_fd); return 1; }

    if (full_pread(g_dev_fd, mft0_buf, g_mft_record_size,
                   (off_t)g_mft_byte_offset) != (ssize_t)g_mft_record_size) {
        fprintf(stderr, "Error: MFT#0 読み取り失敗\n");
        free(mft0_buf); close(g_dev_fd); return 1;
    }
    MFT_RECORD *mft0 = (MFT_RECORD *)mft0_buf;
    if (mft0->magic != MFT_RECORD_MAGIC) {
        fprintf(stderr, "Error: MFT#0 のシグネチャが 'FILE' ではありません\n"
                "  ヒント: losetup -o <byte_offset> でオフセット付き loop デバイスを作成\n");
        free(mft0_buf); close(g_dev_fd); return 1;
    }
    apply_fixup(mft0_buf, g_mft_record_size);

    /* CSV 出力先 */
    FILE *out_fp = stdout;
    if (csv_path) {
        out_fp = fopen(csv_path, "w");
        if (!out_fp) {
            fprintf(stderr, "Error: '%s' を開けません: %s\n",
                    csv_path, strerror(errno));
            free(mft0_buf); close(g_dev_fd); return 1;
        }
    }

    /* CSV ヘッダ（デコードのみ。判定列は持たない） */
    fprintf(out_fp,
            "mft_ref,status,"
            "si_created,si_modified,si_mft_modified,si_accessed,"
            "fn_created,fn_modified,fn_mft_modified,fn_accessed,"
            "filename\n");

    /* 全 MFT レコード走査 */
    fprintf(stderr, "# スキャン中...\n");
    uint64_t cnt_emitted = 0, cnt_total = 0;
    scan_mft(mft0_buf, out_fp, &cnt_emitted, &cnt_total);

    if (out_fp != stdout) fclose(out_fp);
    free(mft0_buf);
    close(g_dev_fd);

    fprintf(stderr, "# ─────────────────────────────────\n");
    fprintf(stderr, "# MFT レコード総数  : %llu\n", (unsigned long long)cnt_total);
    fprintf(stderr, "# 出力レコード数    : %llu\n", (unsigned long long)cnt_emitted);
    if (csv_path)
        fprintf(stderr, "# 出力ファイル      : %s\n", csv_path);

    return 0;
}
