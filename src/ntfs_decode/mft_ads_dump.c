/*
 * mft_ads_dump.c - NTFS MFT 指定 ADS 内容ダンプツール
 *
 * 機能:
 *   MFT# を指定して、そのエントリに付属する ADS (Alternate Data Stream) の
 *   内容を表示する。Zone.Identifier など小さな ADS のテキスト確認に最適。
 *
 * 使い方:
 *   sudo ./mft_ads_dump <デバイス> <MFT#>
 *       → 全 ADS の一覧と内容を表示
 *
 *   sudo ./mft_ads_dump <デバイス> <MFT#> -name Zone.Identifier
 *       → 指定した ADS のみ表示
 *
 *   sudo ./mft_ads_dump <デバイス> <MFT#> -hex
 *       → テキスト表示に加えて hex ダンプも表示
 *
 *   sudo ./mft_ads_dump <デバイス> <MFT#> -name Zone.Identifier -hex
 *       → 指定 ADS を hex ダンプ付きで表示
 *
 * コンパイル:
 *   cc -O2 -Wall -o mft_ads_dump mft_ads_dump.c            # Linux
 *   cc -O2 -Wall -o mft_ads_dump mft_ads_dump.c -liconv    # macOS（iconv を明示リンク）
 *
 * 例:
 *   sudo ./mft_ads_dump /dev/loop0 1234
 *   sudo ./mft_ads_dump /dev/loop0 1234 -name Zone.Identifier -hex
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

#define MAX_NAME_UTF8      768            /* ADS 名の最大 UTF-8 バイト数 */
#define MAX_ADS_ENTRIES    64
#define MAX_RUNS           4096
#define MAX_DUMP_BYTES     (16 * 1024 * 1024)  /* 非常駐 ADS の最大読み込み: 16 MiB */

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
 *  ランリスト解析
 * ============================================================ */
typedef struct { uint64_t lcn; uint64_t run_len; } RunEntry;

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
        if (lbytes == 0 || lbytes > 8 || obytes > 8) break;  /* 不正ニブル(>8/0)でのシフト未定義動作を防ぐ */
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

        if (obytes == 0) {
            runs[n].lcn     = (uint64_t)-1; /* sparse */
            runs[n].run_len = rlen;
        } else {
            cur_lcn += delta;
            runs[n].lcn     = (uint64_t)cur_lcn;
            runs[n].run_len = rlen;
        }
        n++;
    }
    return n;
}

/* ============================================================
 *  非常駐属性のデータを読み込む
 *  戻り値: 確保したバッファ (呼び出し側が free する)、失敗時 NULL
 *  *out_size に実際のデータサイズを返す
 * ============================================================ */
static uint8_t *read_nonres_data(const ATTR_NONRES *anr,
                                  const uint8_t *attr_base,
                                  uint64_t *out_size) {
    uint64_t data_size = anr->data_size;
    if (data_size == 0) {
        *out_size = 0;
        return calloc(1, 1);
    }
    if (data_size > MAX_DUMP_BYTES) {
        fprintf(stderr,
            "[警告] ADS サイズ %llu bytes > 上限 %d bytes。上限まで読み込みます。\n",
            (unsigned long long)data_size, MAX_DUMP_BYTES);
        data_size = MAX_DUMP_BYTES;
    }

    uint8_t *buf = malloc(data_size + 1);
    if (!buf) { perror("malloc"); return NULL; }
    buf[data_size] = '\0';

    const uint8_t *rl_ptr = attr_base + anr->runlist_offset;
    uint32_t rl_len = anr->length - anr->runlist_offset;

    RunEntry runs[MAX_RUNS];
    int num_runs = parse_runlist(rl_ptr, rl_len, runs, MAX_RUNS);

    uint64_t written = 0;
    for (int ri = 0; ri < num_runs && written < data_size; ri++) {
        uint64_t chunk = runs[ri].run_len * g_bytes_per_clus;
        if (written + chunk > data_size)
            chunk = data_size - written;

        if (runs[ri].lcn == (uint64_t)-1) {
            /* sparse: ゼロ埋め */
            memset(buf + written, 0, chunk);
        } else {
            off_t phys = (off_t)(runs[ri].lcn * g_bytes_per_clus);
            ssize_t r = full_pread(g_dev_fd, buf + written, chunk, phys);
            if (r <= 0) {
                fprintf(stderr, "[エラー] 非常駐データ読み込み失敗 (run %d)\n", ri);
                free(buf);
                return NULL;
            }
        }
        written += chunk;
    }

    *out_size = (written < anr->data_size) ? written : anr->data_size;
    return buf;
}

/* ============================================================
 *  表示関数
 * ============================================================ */

/* テキストダンプ: 制御文字は「.」に置換、CR+LF / LF は改行として処理 */
static void print_as_text(const uint8_t *data, uint64_t size) {
    printf("--- テキスト ---\n");
    for (uint64_t i = 0; i < size; i++) {
        uint8_t c = data[i];
        if (c == '\r') {
            /* CR は読み飛ばし (LF と組み合わせて使用されるため) */
            continue;
        } else if (c == '\n') {
            putchar('\n');
        } else if (c >= 0x20 || c == '\t') {
            putchar((int)c);
        } else {
            putchar('.');
        }
    }
    /* 最後に改行がなければ追加 */
    if (size > 0 && data[size - 1] != '\n') putchar('\n');
}

/* hexdump: 16 バイト幅、アドレス + hex + ASCII */
static void print_hexdump(const uint8_t *data, uint64_t size) {
    printf("--- Hex ダンプ ---\n");
    for (uint64_t i = 0; i < size; i += 16) {
        printf("%06llx: ", (unsigned long long)i);
        for (int j = 0; j < 16; j++) {
            if (i + (uint64_t)j < size)
                printf("%02x ", data[i + j]);
            else
                printf("   ");
            if (j == 7) printf(" ");
        }
        printf(" |");
        for (int j = 0; j < 16; j++) {
            if (i + (uint64_t)j >= size) break;
            uint8_t c = data[i + j];
            putchar((c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printf("|\n");
    }
}

/* ============================================================
 *  ADS エントリ
 * ============================================================ */
typedef struct {
    char     name[MAX_NAME_UTF8];
    int      non_resident;
    /* 常駐 */
    const uint8_t *res_data;
    uint32_t       res_size;
    /* 非常駐 */
    const uint8_t *attr_base;  /* 属性先頭ポインタ */
    uint64_t       nonres_data_size;
} AdsInfo;

/* ============================================================
 *  MFT レコード解析: ADS 一覧収集 + ファイル名抽出
 * ============================================================ */
static int collect_ads(const uint8_t *rec_buf,
                        AdsInfo *ads_out, int max_ads,
                        char *fname_out, size_t fname_out_size) {
    const MFT_RECORD *mft = (const MFT_RECORD *)rec_buf;
    int ads_count = 0;
    int best_ns   = -1;

    uint32_t off = mft->attrs_offset;
    while (off + 8 <= g_mft_record_size) {
        uint32_t atype = *(uint32_t *)(rec_buf + off);
        uint32_t alen  = *(uint32_t *)(rec_buf + off + 4);
        if (atype == ATTR_END || alen == 0) break;
        if (off + alen > g_mft_record_size) break;

        const ATTR_RES *ar = (const ATTR_RES *)(rec_buf + off);

        /* 属性名 (ADS 判定) */
        char attr_name[MAX_NAME_UTF8] = "";
        if (ar->name_length > 0 &&
            ar->name_offset + (uint32_t)ar->name_length * 2 <= alen) {
            utf16le_to_utf8(rec_buf + off + ar->name_offset,
                            ar->name_length * 2,
                            attr_name, sizeof(attr_name));
        }

        /* $FILE_NAME: ファイル名抽出 */
        if (atype == ATTR_FILE_NAME && !ar->non_resident &&
            ar->value_length >= (uint32_t)sizeof(FILE_NAME_ATTR) &&
            ar->value_offset + ar->value_length <= alen) {

            const uint8_t *val = rec_buf + off + ar->value_offset;
            const FILE_NAME_ATTR *fna = (const FILE_NAME_ATTR *)val;
            int ns = fna->name_namespace;

            /* Win32 (1) > Win32&DOS (3) > POSIX (0) > DOS (2) */
            if (best_ns < 0 ||
                (ns == 1) ||
                (ns == 3 && best_ns != 1) ||
                (ns == 0 && best_ns == 2)) {
                best_ns = ns;
                uint32_t nbytes = fna->name_length * 2u;
                if (sizeof(FILE_NAME_ATTR) + nbytes <= ar->value_length) {
                    utf16le_to_utf8(val + sizeof(FILE_NAME_ATTR), nbytes,
                                    fname_out, fname_out_size);
                }
            }
        }

        /* $DATA で名前あり = ADS */
        if (atype == ATTR_DATA && attr_name[0] != '\0') {
            if (ads_count < max_ads) {
                AdsInfo *a = &ads_out[ads_count];
                snprintf(a->name, MAX_NAME_UTF8, "%s", attr_name);

                if (!ar->non_resident) {
                    /* 常駐 */
                    if (ar->value_offset + ar->value_length <= alen) {
                        a->non_resident = 0;
                        a->res_data     = rec_buf + off + ar->value_offset;
                        a->res_size     = ar->value_length;
                        a->attr_base    = NULL;
                        a->nonres_data_size = 0;
                        ads_count++;
                    }
                } else {
                    /* 非常駐 */
                    const ATTR_NONRES *anr = (const ATTR_NONRES *)(rec_buf + off);
                    a->non_resident     = 1;
                    a->res_data         = NULL;
                    a->res_size         = 0;
                    a->attr_base        = rec_buf + off;
                    a->nonres_data_size = anr->data_size;
                    ads_count++;
                }
            }
        }

        off += alen;
    }
    return ads_count;
}

/* ============================================================
 *  1 つの ADS を表示する
 * ============================================================ */
static void dump_one_ads(const AdsInfo *a, const char *fname,
                          int show_hex) {
    uint64_t    size = 0;
    const uint8_t *data_ptr = NULL;
    uint8_t    *alloc_buf   = NULL;

    if (!a->non_resident) {
        data_ptr = a->res_data;
        size     = a->res_size;
    } else {
        const ATTR_NONRES *anr = (const ATTR_NONRES *)a->attr_base;
        alloc_buf = read_nonres_data(anr, a->attr_base, &size);
        if (!alloc_buf) {
            fprintf(stderr, "[エラー] 非常駐データ読み込み失敗: %s\n", a->name);
            return;
        }
        data_ptr = alloc_buf;
    }

    printf("\n");
    printf("--------------------------------------------\n");
    printf("  ADS : %s:%s\n", fname, a->name);
    printf("  種別: %s\n", a->non_resident ? "非常駐 (Non-Resident)" : "常駐 (Resident)");
    printf("  サイズ: %llu bytes\n", (unsigned long long)size);
    printf("--------------------------------------------\n");

    if (size == 0) {
        printf("[空の ADS]\n");
    } else {
        print_as_text(data_ptr, size);
        if (show_hex) {
            print_hexdump(data_ptr, size);
        }
    }

    if (alloc_buf) free(alloc_buf);
}

/* ============================================================
 *  MFT#0 のランリストを使って対象レコードの物理オフセットを求める
 *
 *  MFT が断片化している場合、単純な
 *    g_mft_byte_offset + MFT# * g_mft_record_size
 *  では正しい位置を指せない。MFT#0 の $DATA ランリストを辿って
 *  VCN (= レコード番号 × レコードサイズ / クラスタサイズ) に
 *  対応する物理オフセットを返す。
 *
 *  戻り値: 物理バイトオフセット、失敗時 (off_t)-1
 * ============================================================ */
static off_t locate_mft_record(uint64_t mft_no) {
    /* ── MFT#0 を読み込む ── */
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

    /* ── MFT#0 の無名 $DATA ランリストを取得 ── */
    const uint8_t  *rl_ptr = NULL;
    uint32_t        rl_len = 0;

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

    RunEntry runs[MAX_RUNS];
    int num_runs = parse_runlist(rl_ptr, rl_len, runs, MAX_RUNS);
    free(mft0_buf);

    if (num_runs == 0) {
        fprintf(stderr, "Error: MFT ランリストが空です\n");
        return (off_t)-1;
    }

    /* ── ランあたりのレコード数を計算 ──
     *
     *  [Case A] クラスタ >= レコード (例: 4096B / 1024B = 4 rec/clus)
     *    recs_in_run = run_len × (clus_size / rec_size)
     *
     *  [Case B] クラスタ < レコード (例: 512B < 1024B → 1レコード = 2クラスタ)
     *    recs_in_run = run_len / (rec_size / clus_size)
     *
     *  物理オフセットはどちらも:
     *    lcn × clus_size + local_rec × rec_size
     */
    uint64_t target_rec_idx = mft_no;
    uint64_t rec_idx = 0;

    for (int ri = 0; ri < num_runs; ri++) {
        uint64_t recs_in_run;
        if (g_bytes_per_clus >= g_mft_record_size) {
            /* Case A: 1クラスタに複数レコード */
            recs_in_run = runs[ri].run_len *
                          (g_bytes_per_clus / g_mft_record_size);
        } else {
            /* Case B: 1レコードが複数クラスタにまたがる */
            uint32_t clus_per_rec = g_mft_record_size / g_bytes_per_clus;
            recs_in_run = runs[ri].run_len / clus_per_rec;
        }

        if (target_rec_idx < rec_idx + recs_in_run) {
            /* このランの中にある */
            uint64_t local_rec = target_rec_idx - rec_idx;

            if (runs[ri].lcn == (uint64_t)-1) {
                /* sparse: MFT レコードが sparse になることは通常ない */
                fprintf(stderr, "Error: MFT#%llu が sparse ランに含まれています\n",
                        (unsigned long long)mft_no);
                return (off_t)-1;
            }

            off_t phys = (off_t)(runs[ri].lcn * g_bytes_per_clus
                                  + local_rec * g_mft_record_size);
            return phys;
        }
        rec_idx += recs_in_run;
    }

    fprintf(stderr,
        "Error: MFT#%llu はランリストの範囲外です (ランリスト内レコード数: %llu)\n",
        (unsigned long long)mft_no, (unsigned long long)rec_idx);
    return (off_t)-1;
}

/* ============================================================
 *  main
 * ============================================================ */
int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    /* ── 引数解析 ── */
    if (argc < 3) {
        fprintf(stderr,
            "使い方: %s <デバイス> <MFT#> [-name <ADS名>] [-hex]\n"
            "\n"
            "  例:\n"
            "    sudo %s /dev/loop0 1234\n"
            "                    → 全 ADS を表示\n"
            "    sudo %s /dev/loop0 1234 -name Zone.Identifier\n"
            "                    → Zone.Identifier のみ表示\n"
            "    sudo %s /dev/loop0 1234 -name Zone.Identifier -hex\n"
            "                    → hex ダンプも表示\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *device    = argv[1];
    uint64_t    target_mft = (uint64_t)strtoull(argv[2], NULL, 10);
    const char *filter_name = NULL;
    int         show_hex    = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            filter_name = argv[++i];
        } else if (strcmp(argv[i], "-hex") == 0) {
            show_hex = 1;
        } else {
            fprintf(stderr, "[警告] 不明なオプション: %s\n", argv[i]);
        }
    }

    /* ── デバイスオープン ── */
    g_dev_fd = open(device, O_RDONLY);
    if (g_dev_fd < 0) {
        fprintf(stderr, "Error: '%s' を開けません: %s\n", device, strerror(errno));
        return 1;
    }

    /* ── ブートセクタ ── */
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

    fprintf(stderr, "# デバイス          : %s\n",  device);
    fprintf(stderr, "# クラスタサイズ    : %u B\n", g_bytes_per_clus);
    fprintf(stderr, "# MFT レコードサイズ: %u B\n", g_mft_record_size);
    fprintf(stderr, "# 対象 MFT#         : %llu\n",
            (unsigned long long)target_mft);

    /* ── 対象 MFT レコード読み込み ──
     *  MFT が断片化している場合に備え、MFT#0 のランリストを辿って
     *  物理オフセットを求める。
     */
    uint8_t *rec_buf = malloc(g_mft_record_size);
    if (!rec_buf) { perror("malloc"); close(g_dev_fd); return 1; }

    off_t rec_offset = locate_mft_record(target_mft);
    if (rec_offset == (off_t)-1) {
        free(rec_buf); close(g_dev_fd); return 1;
    }
    fprintf(stderr, "# MFT#%llu 物理オフセット: 0x%llx\n",
            (unsigned long long)target_mft,
            (unsigned long long)rec_offset);

    ssize_t r = full_pread(g_dev_fd, rec_buf, g_mft_record_size, rec_offset);
    if (r != (ssize_t)g_mft_record_size) {
        fprintf(stderr, "Error: MFT#%llu 読み取り失敗\n",
                (unsigned long long)target_mft);
        free(rec_buf); close(g_dev_fd); return 1;
    }

    MFT_RECORD *mft = (MFT_RECORD *)rec_buf;
    if (mft->magic != MFT_RECORD_MAGIC) {
        fprintf(stderr,
            "Error: MFT#%llu のシグネチャが 'FILE' ではありません\n"
            "  ヒント: losetup -o <byte_offset> でパーティション先頭を指定してください\n",
            (unsigned long long)target_mft);
        free(rec_buf); close(g_dev_fd); return 1;
    }

    apply_fixup(rec_buf, g_mft_record_size);

    const char *status = (mft->flags & MFT_RECORD_IN_USE) ? "IN_USE" : "DELETED";
    fprintf(stderr, "# レコード状態      : %s\n", status);

    /* ── ADS 収集 ── */
    AdsInfo ads[MAX_ADS_ENTRIES];
    char    fname[MAX_NAME_UTF8] = "(不明)";
    int ads_count = collect_ads(rec_buf, ads, MAX_ADS_ENTRIES,
                                 fname, sizeof(fname));

    fprintf(stderr, "# ファイル名        : %s\n", fname);
    fprintf(stderr, "# ADS 数            : %d\n", ads_count);

    /* ── 表示 ── */
    if (ads_count == 0) {
        printf("[MFT#%llu: %s] ADS は存在しません\n",
               (unsigned long long)target_mft, fname);
        free(rec_buf); close(g_dev_fd); return 0;
    }

    int found = 0;
    for (int i = 0; i < ads_count; i++) {
        if (filter_name && strcmp(ads[i].name, filter_name) != 0)
            continue;
        dump_one_ads(&ads[i], fname, show_hex);
        found++;
    }

    if (filter_name && found == 0) {
        printf("[MFT#%llu: %s] ADS '%s' は見つかりませんでした\n",
               (unsigned long long)target_mft, fname, filter_name);
        printf("存在する ADS 一覧:\n");
        for (int i = 0; i < ads_count; i++)
            printf("  - %s (%s, %llu bytes)\n",
                   ads[i].name,
                   ads[i].non_resident ? "非常駐" : "常駐",
                   ads[i].non_resident
                       ? (unsigned long long)ads[i].nonres_data_size
                       : (unsigned long long)ads[i].res_size);
    }

    free(rec_buf);
    close(g_dev_fd);
    return 0;
}
