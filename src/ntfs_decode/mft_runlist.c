/*
 * mft_runlist.c - NTFS 非常駐属性のランリスト(DATA RUN)解読ツール
 *
 * 機能:
 *   MFT# を指定して、そのエントリの非常駐 $DATA 属性に格納された
 *   ランリスト(DATA RUN)を解読し、各ラン(連続クラスタのかたまり)を
 *   「ヘッダバイト・length・delta・絶対LCN・VCN範囲」で一覧表示する。
 *   本(DATA RUN の章)で dd|xxd して手計算した結果の「答え合わせ」に使う。
 *
 *   ランリストそのものを出力するのはこのツールだけ。mft_decode.c /
 *   mft_ads_dump.c / mft_tm_decode.c は parse_runlist() を内部で使って
 *   データの位置を特定するだけで、ランを人間可読で出力はしない。
 *
 * 使い方:
 *   sudo ./mft_runlist <デバイス> <MFT#>
 *       → 無名 $DATA のランリストを表示 (例: $MFT=0, 一般ファイル)
 *
 *   sudo ./mft_runlist <デバイス> <MFT#> -name <ADS名>
 *       → 名前付き $DATA (ADS) のランリストを表示
 *
 * コンパイル:
 *   cc -O2 -Wall -o mft_runlist mft_runlist.c            # Linux
 *   cc -O2 -Wall -o mft_runlist mft_runlist.c -liconv    # macOS
 *
 * 例:
 *   sudo ./mft_runlist /dev/loop0p1 0        # $MFT 自身のランリスト
 *   sudo ./mft_runlist /dev/loop0p1 45       # 一般ファイル
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <iconv.h>
#include <locale.h>

/* ============================================================
 *  定数
 * ============================================================ */
#define MFT_RECORD_MAGIC   0x454C4946U   /* "FILE" */
#define MFT_RECORD_IN_USE  0x0001

#define ATTR_FILE_NAME     0x30U
#define ATTR_DATA          0x80U
#define ATTR_END           0xFFFFFFFFU

#define MAX_NAME_UTF8      768
#define MAX_RUNS           4096

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

/* 常駐属性ヘッダ */
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

/* 非常駐属性ヘッダ */
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

/* $FILE_NAME 属性値 */
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
} FILE_NAME_ATTR;

#pragma pack(pop)

/* ============================================================
 *  グローバル (ボリューム情報)
 * ============================================================ */
static int      g_dev_fd          = -1;
static uint32_t g_bytes_per_sec   = 512;
static uint32_t g_bytes_per_clus  = 4096;
static uint32_t g_mft_record_size = 1024;
static uint64_t g_mft_byte_offset = 0;

/* ============================================================
 *  ユーティリティ
 * ============================================================ */

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

static void apply_fixup(uint8_t *rec, uint32_t rec_size) {
    MFT_RECORD *mft = (MFT_RECORD *)rec;
    if (mft->usa_offset == 0 || mft->usa_count < 2) return;
    uint16_t *usa = (uint16_t *)(rec + mft->usa_offset);
    uint32_t  n   = mft->usa_count - 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = (i + 1) * g_bytes_per_sec - 2;
        if (off + 2 > rec_size) break;
        uint16_t *p = (uint16_t *)(rec + off);
        *p = usa[i + 1];
    }
}

static int utf16le_to_utf8(const uint8_t *src, size_t src_bytes,
                            char *dst, size_t dst_bytes) {
    if (src_bytes == 0) { dst[0] = '\0'; return 0; }
    iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
    if (cd == (iconv_t)-1) { dst[0] = '\0'; return -1; }
    char  *in  = (char *)src;
    char  *out = dst;
    size_t inl = src_bytes, outl = dst_bytes - 1;
    iconv(cd, &in, &inl, &out, &outl);
    *out = '\0';
    iconv_close(cd);
    return 0;
}

/* ============================================================
 *  ランリスト解析 (DATA RUN の章の parse_runlist() と同一ロジック)
 *
 *  ここでは本の手計算と突き合わせられるよう、絶対LCNだけでなく
 *  ヘッダバイト・length・delta も記録する。
 * ============================================================ */
typedef struct {
    uint8_t  hdr;        /* ヘッダバイト */
    int      lbytes;     /* length フィールドのバイト数 */
    int      obytes;     /* offset フィールドのバイト数 */
    uint64_t run_len;    /* クラスタ数 */
    int64_t  delta;      /* 前ランからのデルタ LCN */
    uint64_t lcn;        /* 絶対LCN (スパースは (uint64_t)-1) */
    uint64_t start_vcn;  /* このランの開始 VCN */
} RunEntry;

static int parse_runlist_verbose(const uint8_t *rl, size_t max_len,
                                  RunEntry *runs, int max_runs) {
    int     n       = 0;
    size_t  pos     = 0;
    int64_t cur_lcn = 0;
    uint64_t vcn    = 0;

    while (pos < max_len && n < max_runs) {
        uint8_t hdr = rl[pos++];
        if (hdr == 0) break;               /* ターミネータ */

        int lbytes = hdr & 0x0F;
        int obytes = (hdr >> 4) & 0x0F;
        if (pos + (size_t)(lbytes + obytes) > max_len) break;

        uint64_t rlen = 0;
        for (int i = 0; i < lbytes; i++)
            rlen |= (uint64_t)rl[pos++] << (i * 8);

        int64_t delta = 0;
        for (int i = 0; i < obytes; i++)
            delta |= (int64_t)rl[pos++] << (i * 8);
        /* 符号拡張 */
        if (obytes > 0 && (rl[pos - 1] & 0x80))
            for (int i = obytes; i < 8; i++)
                delta |= (int64_t)0xFF << (i * 8);

        runs[n].hdr       = hdr;
        runs[n].lbytes    = lbytes;
        runs[n].obytes    = obytes;
        runs[n].run_len   = rlen;
        runs[n].delta     = delta;
        runs[n].start_vcn = vcn;

        if (obytes == 0) {
            runs[n].lcn = (uint64_t)-1;    /* スパース */
        } else {
            cur_lcn += delta;
            runs[n].lcn = (uint64_t)cur_lcn;
        }
        vcn += rlen;
        n++;
    }
    return n;
}

/* 位置特定用: 絶対LCNとクラスタ数だけ返す簡易版 */
typedef struct { uint64_t lcn; uint64_t run_len; } SimpleRun;

static int parse_runlist_simple(const uint8_t *rl, size_t max_len,
                                 SimpleRun *runs, int max_runs) {
    int n = 0; size_t pos = 0; int64_t cur_lcn = 0;
    while (pos < max_len && n < max_runs) {
        uint8_t hdr = rl[pos++];
        if (hdr == 0) break;
        int lbytes = hdr & 0x0F;
        int obytes = (hdr >> 4) & 0x0F;
        if (pos + (size_t)(lbytes + obytes) > max_len) break;
        uint64_t rlen = 0;
        for (int i = 0; i < lbytes; i++)
            rlen |= (uint64_t)rl[pos++] << (i * 8);
        int64_t delta = 0;
        for (int i = 0; i < obytes; i++)
            delta |= (int64_t)rl[pos++] << (i * 8);
        if (obytes > 0 && (rl[pos - 1] & 0x80))
            for (int i = obytes; i < 8; i++)
                delta |= (int64_t)0xFF << (i * 8);
        if (obytes == 0) { runs[n].lcn = (uint64_t)-1; runs[n].run_len = rlen; }
        else { cur_lcn += delta; runs[n].lcn = (uint64_t)cur_lcn; runs[n].run_len = rlen; }
        n++;
    }
    return n;
}

/* ============================================================
 *  MFT#0 のランリストを使って対象レコードの物理オフセットを求める
 *  (mft_ads_dump.c の locate_mft_record と同じ)
 * ============================================================ */
static off_t locate_mft_record(uint64_t mft_no) {
    uint8_t *mft0_buf = malloc(g_mft_record_size);
    if (!mft0_buf) { perror("malloc"); return (off_t)-1; }

    ssize_t r = full_pread(g_dev_fd, mft0_buf, g_mft_record_size,
                            (off_t)g_mft_byte_offset);
    if (r != (ssize_t)g_mft_record_size) {
        fprintf(stderr, "Error: MFT#0 読み取り失敗\n");
        free(mft0_buf); return (off_t)-1;
    }
    MFT_RECORD *mft0 = (MFT_RECORD *)mft0_buf;
    if (mft0->magic != MFT_RECORD_MAGIC) {
        fprintf(stderr, "Error: MFT#0 シグネチャ不正\n");
        free(mft0_buf); return (off_t)-1;
    }
    apply_fixup(mft0_buf, g_mft_record_size);

    const uint8_t *rl_ptr = NULL;
    uint32_t       rl_len = 0;

    uint32_t off = mft0->attrs_offset;
    while (off + 8 <= g_mft_record_size) {
        uint32_t atype = *(uint32_t *)(mft0_buf + off);
        uint32_t alen  = *(uint32_t *)(mft0_buf + off + 4);
        if (atype == ATTR_END || alen == 0) break;
        if (off + alen > g_mft_record_size) break;

        if (atype == ATTR_DATA) {
            const ATTR_NONRES *anr = (const ATTR_NONRES *)(mft0_buf + off);
            if (anr->non_resident && anr->name_length == 0) {
                rl_ptr = mft0_buf + off + anr->runlist_offset;
                rl_len = alen - anr->runlist_offset;
                break;
            }
        }
        off += alen;
    }
    if (!rl_ptr) {
        fprintf(stderr, "Error: MFT#0 の $DATA ランリストが見つかりません\n");
        free(mft0_buf); return (off_t)-1;
    }

    SimpleRun runs[MAX_RUNS];
    int num_runs = parse_runlist_simple(rl_ptr, rl_len, runs, MAX_RUNS);
    free(mft0_buf);
    if (num_runs == 0) {
        fprintf(stderr, "Error: MFT ランリストが空です\n");
        return (off_t)-1;
    }

    uint64_t target_rec_idx = mft_no;
    uint64_t rec_idx = 0;
    for (int ri = 0; ri < num_runs; ri++) {
        uint64_t recs_in_run;
        if (g_bytes_per_clus >= g_mft_record_size)
            recs_in_run = runs[ri].run_len *
                          (g_bytes_per_clus / g_mft_record_size);
        else {
            uint32_t clus_per_rec = g_mft_record_size / g_bytes_per_clus;
            recs_in_run = runs[ri].run_len / clus_per_rec;
        }
        if (target_rec_idx < rec_idx + recs_in_run) {
            uint64_t local_rec = target_rec_idx - rec_idx;
            if (runs[ri].lcn == (uint64_t)-1) {
                fprintf(stderr, "Error: MFT#%llu が sparse ランに含まれています\n",
                        (unsigned long long)mft_no);
                return (off_t)-1;
            }
            return (off_t)(runs[ri].lcn * g_bytes_per_clus
                            + local_rec * g_mft_record_size);
        }
        rec_idx += recs_in_run;
    }
    fprintf(stderr,
        "Error: MFT#%llu はランリストの範囲外です\n",
        (unsigned long long)mft_no);
    return (off_t)-1;
}

/* ============================================================
 *  対象属性($DATA)を見つけてランリストを表示
 * ============================================================ */
static int dump_runlist(const uint8_t *rec_buf, const char *filter_name,
                        const char *fname) {
    const MFT_RECORD *mft = (const MFT_RECORD *)rec_buf;

    uint32_t off = mft->attrs_offset;
    while (off + 8 <= g_mft_record_size) {
        uint32_t atype = *(uint32_t *)(rec_buf + off);
        uint32_t alen  = *(uint32_t *)(rec_buf + off + 4);
        if (atype == ATTR_END || alen == 0) break;
        if (off + alen > g_mft_record_size) break;

        if (atype != ATTR_DATA) { off += alen; continue; }

        const ATTR_RES *ar = (const ATTR_RES *)(rec_buf + off);

        /* 属性名 */
        char attr_name[MAX_NAME_UTF8] = "";
        if (ar->name_length > 0 &&
            ar->name_offset + (uint32_t)ar->name_length * 2 <= alen) {
            utf16le_to_utf8(rec_buf + off + ar->name_offset,
                            ar->name_length * 2, attr_name, sizeof(attr_name));
        }

        /* フィルタ: -name 指定時はその ADS 名、未指定時は無名 $DATA */
        if (filter_name) {
            if (strcmp(attr_name, filter_name) != 0) { off += alen; continue; }
        } else {
            if (attr_name[0] != '\0') { off += alen; continue; }
        }

        /* 対象 $DATA を発見 */
        char label[MAX_NAME_UTF8 + 32];
        if (attr_name[0] != '\0')
            snprintf(label, sizeof(label), "$DATA:%s (ADS)", attr_name);
        else
            snprintf(label, sizeof(label), "$DATA (無名)");

        if (!ar->non_resident) {
            printf("# 属性              : %s\n", label);
            printf("# 格納              : 常駐 (データは MFT レコード内、ランリストなし)\n");
            printf("# サイズ            : %u bytes\n", ar->value_length);
            return 0;
        }

        const ATTR_NONRES *anr = (const ATTR_NONRES *)(rec_buf + off);
        const uint8_t *rl_ptr  = rec_buf + off + anr->runlist_offset;
        uint32_t rl_len = alen - anr->runlist_offset;

        /* ランリスト生バイト列 (ターミネータ 0x00 まで) を求める */
        uint32_t rl_used = 0;
        {
            size_t p = 0;
            while (p < rl_len) {
                uint8_t h = rl_ptr[p];
                if (h == 0) { p++; break; }
                int lb = h & 0x0F, ob = (h >> 4) & 0x0F;
                p += 1 + lb + ob;
            }
            rl_used = (p <= rl_len) ? (uint32_t)p : rl_len;
        }

        printf("# 属性              : %s\n", label);
        printf("# 格納              : 非常駐 (Non-Resident)\n");
        printf("# data_size         : %llu bytes (%llu クラスタ)\n",
               (unsigned long long)anr->data_size,
               (unsigned long long)((anr->data_size + g_bytes_per_clus - 1)
                                     / g_bytes_per_clus));
        printf("# runlist_offset    : 0x%x\n", anr->runlist_offset);
        printf("# ランリスト生バイト: ");
        for (uint32_t i = 0; i < rl_used; i++)
            printf("%02x ", rl_ptr[i]);
        printf("\n\n");

        RunEntry runs[MAX_RUNS];
        int nr = parse_runlist_verbose(rl_ptr, rl_len, runs, MAX_RUNS);

        printf("run  hdr   length    delta       LCN       VCN範囲\n");
        printf("---  ----  --------  ----------  --------  ----------\n");
        uint64_t total_clus = 0;
        for (int i = 0; i < nr; i++) {
            uint64_t vcn_end = runs[i].start_vcn + runs[i].run_len - 1;
            if (runs[i].lcn == (uint64_t)-1) {
                printf("%3d  0x%02x  %8llu  %10s  %-8s  %llu-%llu\n",
                       i + 1, runs[i].hdr,
                       (unsigned long long)runs[i].run_len,
                       "-", "sparse",
                       (unsigned long long)runs[i].start_vcn,
                       (unsigned long long)vcn_end);
            } else {
                char deltabuf[16];
                snprintf(deltabuf, sizeof(deltabuf), "%+lld",
                         (long long)runs[i].delta);
                printf("%3d  0x%02x  %8llu  %10s  %8llu  %llu-%llu\n",
                       i + 1, runs[i].hdr,
                       (unsigned long long)runs[i].run_len,
                       deltabuf,
                       (unsigned long long)runs[i].lcn,
                       (unsigned long long)runs[i].start_vcn,
                       (unsigned long long)vcn_end);
            }
            total_clus += runs[i].run_len;
        }
        printf("---  ----  --------  ----------  --------  ----------\n");

        uint64_t total_bytes = total_clus * g_bytes_per_clus;
        char match[80];
        if (anr->data_size <= total_bytes &&
            anr->data_size >  total_bytes - g_bytes_per_clus)
            snprintf(match, sizeof match,
                     " → data_size %llu B を格納（割り当て内）",
                     (unsigned long long)anr->data_size);
        else
            match[0] = '\0';
        printf("合計: %d ラン, %llu クラスタ, %llu bytes%s\n",
               nr, (unsigned long long)total_clus,
               (unsigned long long)total_bytes, match);
        return 0;
    }

    if (filter_name)
        printf("[MFT#?: %s] 非常駐 $DATA:%s は見つかりませんでした\n",
               fname, filter_name);
    else
        printf("[%s] 非常駐の無名 $DATA が見つかりませんでした"
               " (常駐ファイル、またはディレクトリの可能性)\n", fname);
    return 1;
}

/* ============================================================
 *  main
 * ============================================================ */
int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    if (argc < 3) {
        fprintf(stderr,
            "使い方: %s <デバイス> <MFT#> [-name <ADS名>]\n"
            "\n"
            "  例:\n"
            "    sudo %s /dev/loop0p1 0     # $MFT 自身のランリスト\n"
            "    sudo %s /dev/loop0p1 45    # 一般ファイルの $DATA\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *device     = argv[1];
    uint64_t    target_mft  = (uint64_t)strtoull(argv[2], NULL, 10);
    const char *filter_name = NULL;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc)
            filter_name = argv[++i];
        else
            fprintf(stderr, "[警告] 不明なオプション: %s\n", argv[i]);
    }

    g_dev_fd = open(device, O_RDONLY);
    if (g_dev_fd < 0) {
        fprintf(stderr, "Error: '%s' を開けません: %s\n", device, strerror(errno));
        return 1;
    }

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

    g_bytes_per_sec  = boot.bytes_per_sector;
    g_bytes_per_clus = g_bytes_per_sec * boot.sectors_per_cluster;
    if (boot.clusters_per_mft_record < 0)
        g_mft_record_size = 1u << (uint8_t)(-boot.clusters_per_mft_record);
    else if (boot.clusters_per_mft_record > 0)
        g_mft_record_size = (uint32_t)boot.clusters_per_mft_record * g_bytes_per_clus;
    else
        g_mft_record_size = 1024;
    g_mft_byte_offset = boot.mft_lcn * g_bytes_per_clus;

    printf("# デバイス          : %s\n",  device);
    printf("# クラスタサイズ    : %u B\n", g_bytes_per_clus);
    printf("# MFT レコードサイズ: %u B\n", g_mft_record_size);
    printf("# 対象 MFT#         : %llu\n", (unsigned long long)target_mft);

    uint8_t *rec_buf = malloc(g_mft_record_size);
    if (!rec_buf) { perror("malloc"); close(g_dev_fd); return 1; }

    off_t rec_offset = locate_mft_record(target_mft);
    if (rec_offset == (off_t)-1) { free(rec_buf); close(g_dev_fd); return 1; }
    printf("# MFT#%llu 物理オフセット: 0x%llx\n",
           (unsigned long long)target_mft, (unsigned long long)rec_offset);

    ssize_t r = full_pread(g_dev_fd, rec_buf, g_mft_record_size, rec_offset);
    if (r != (ssize_t)g_mft_record_size) {
        fprintf(stderr, "Error: MFT#%llu 読み取り失敗\n",
                (unsigned long long)target_mft);
        free(rec_buf); close(g_dev_fd); return 1;
    }

    MFT_RECORD *mft = (MFT_RECORD *)rec_buf;
    if (mft->magic != MFT_RECORD_MAGIC) {
        fprintf(stderr,
            "Error: MFT#%llu のシグネチャが 'FILE' ではありません\n",
            (unsigned long long)target_mft);
        free(rec_buf); close(g_dev_fd); return 1;
    }
    apply_fixup(rec_buf, g_mft_record_size);

    const char *status = (mft->flags & MFT_RECORD_IN_USE) ? "IN_USE" : "DELETED";
    printf("# レコード状態      : %s\n", status);

    /* ファイル名抽出 (表示用) */
    char fname[MAX_NAME_UTF8] = "(不明)";
    {
        int best_ns = -1;
        uint32_t off = mft->attrs_offset;
        while (off + 8 <= g_mft_record_size) {
            uint32_t atype = *(uint32_t *)(rec_buf + off);
            uint32_t alen  = *(uint32_t *)(rec_buf + off + 4);
            if (atype == ATTR_END || alen == 0) break;
            if (off + alen > g_mft_record_size) break;
            const ATTR_RES *ar = (const ATTR_RES *)(rec_buf + off);
            if (atype == ATTR_FILE_NAME && !ar->non_resident &&
                ar->value_length >= (uint32_t)sizeof(FILE_NAME_ATTR) &&
                ar->value_offset + ar->value_length <= alen) {
                const uint8_t *val = rec_buf + off + ar->value_offset;
                const FILE_NAME_ATTR *fna = (const FILE_NAME_ATTR *)val;
                int ns = fna->name_namespace;
                if (best_ns < 0 || ns == 1 ||
                    (ns == 3 && best_ns != 1) || (ns == 0 && best_ns == 2)) {
                    best_ns = ns;
                    uint32_t nbytes = fna->name_length * 2u;
                    if (sizeof(FILE_NAME_ATTR) + nbytes <= ar->value_length)
                        utf16le_to_utf8(val + sizeof(FILE_NAME_ATTR), nbytes,
                                        fname, sizeof(fname));
                }
            }
            off += alen;
        }
    }
    printf("# ファイル名        : %s\n", fname);

    int rc = dump_runlist(rec_buf, filter_name, fname);

    free(rec_buf);
    close(g_dev_fd);
    return rc;
}
