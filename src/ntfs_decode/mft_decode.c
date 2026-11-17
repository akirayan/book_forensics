/*
 * mft_decode.c - NTFS MFT Parser
 *
 * 全 MFT レコードを辿り、ファイル名・フルパス・タイムスタンプ・サイズを CSV 化する。
 *   - file_size 列: 通常ファイルは無名 $DATA の data_size (論理バイト数)、
 *                   ADS は名前付き $DATA の data_size (各ストリームのバイト数)、
 *                   DIR は 0。常駐 $DATA (小さなファイル) も非常駐も両方対応。
 *
 * 列順:
 *   mft_ref, parent_mft_ref, status, type,
 *   modified_tm, accessed_tm, created_tm, filename, fullpath, file_size
 *
 * コンパイル:
 *   cc -O2 -Wall -o mft_decode mft_decode.c            # Linux
 *   cc -O2 -Wall -o mft_decode mft_decode.c -liconv    # macOS（iconv を明示リンク）
 *
 * 実行:
 *   sudo ./mft_decode /dev/loop0p1
 *   sudo ./mft_decode /dev/loop0p1 out.csv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <iconv.h>
#include <locale.h>

/* ============================================================
 *  定数
 * ============================================================ */
#define MFT_RECORD_MAGIC    0x454C4946U
#define MFT_RECORD_IN_USE   0x0001
#define MFT_RECORD_IS_DIR   0x0002

#define ATTR_STANDARD_INFORMATION  0x10U
#define ATTR_FILE_NAME             0x30U
#define ATTR_REPARSE_POINT         0xC0U
#define ATTR_DATA                  0x80U
#define ATTR_END                   0xFFFFFFFFU

#define FA_DIRECTORY    0x00000010U
#define FA_REPARSE      0x00000400U

#define FILE_NAME_POSIX     0
#define FILE_NAME_WIN32     1
#define FILE_NAME_DOS       2
#define FILE_NAME_WIN32DOS  3

#define MAX_FN_ENTRIES   16
#define MAX_ADS_ENTRIES  64
#define MAX_NAME_UTF8    768    /* 255 UTF-16 → max 765 UTF-8 bytes */
#define MAX_PATH_UTF8    4096   /* フルパスの最大長 */

/* MFT# 5 = ルートディレクトリ "." */
#define MFT_ROOT         5ULL

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

typedef struct {
    uint64_t created;
    uint64_t modified;
    uint64_t mft_modified;
    uint64_t accessed;
} STD_INFO;

typedef struct {
    uint64_t parent_ref;    /* 下位 48bit = 親 MFT#, 上位 16bit = 親 SeqNo */
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
 *  ディレクトリテーブル (Pass 1 で構築)
 *
 *  MFT# をキーにした配列。
 *  フルパス解決のために全エントリ（ファイルも含む）を記録する。
 * ============================================================ */
typedef struct {
    uint64_t mft_no;        /* このエントリの MFT# */
    uint64_t parent_mft_no; /* 親ディレクトリの MFT# */
    char     name[MAX_NAME_UTF8]; /* Win32 優先のファイル名 */
    uint8_t  valid;         /* 1=有効エントリ */
} DirEntry;

static DirEntry  *g_dir_table  = NULL;  /* 動的配列 */
static uint64_t   g_dir_cap    = 0;     /* 確保済み要素数 */
static uint64_t   g_dir_count  = 0;     /* 使用済み要素数 */

/* MFT# → DirEntry のインデックス (直接アクセス配列)
   g_mft_index[mft_no] = g_dir_table のインデックス (UINT64_MAX = 未登録) */
static uint64_t  *g_mft_index  = NULL;
static uint64_t   g_mft_index_cap = 0;

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

static void ntfs_time_to_str(uint64_t ts, char *buf, size_t buflen) {
    if (ts == 0) { buf[0] = '\0'; return; }
    uint32_t sub7 = (uint32_t)(ts % 10000000ULL);
    time_t t = (time_t)(ts / 10000000ULL) - 11644473600ULL;
    struct tm *tm = gmtime(&t);
    if (!tm) { buf[0] = '\0'; return; }
    char base[32];
    strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", tm);
    snprintf(buf, buflen, "%s.%07u", base, sub7);
}

static int utf16le_to_utf8(const uint8_t *src, size_t src_bytes,
                            char *dst, size_t dst_bytes) {
    if (src_bytes == 0) { dst[0] = '\0'; return 0; }
    iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
    if (cd == (iconv_t)-1) { dst[0] = '\0'; return -1; }
    char   *in  = (char *)src;
    char   *out = dst;
    size_t  inl = src_bytes;
    size_t  outl = dst_bytes - 1;
    iconv(cd, &in, &inl, &out, &outl);
    *out = '\0';
    iconv_close(cd);
    return 0;
}

static void csv_write_field(FILE *fp, const char *s) {
    fputc('"', fp);
    for (; *s; s++) {
        if (*s == '"') fputc('"', fp);
        fputc(*s, fp);
    }
    fputc('"', fp);
}

/* ============================================================
 *  Fix-up
 * ============================================================ */
static void apply_fixup(uint8_t *rec, uint32_t rec_size) {
    MFT_RECORD *mft = (MFT_RECORD *)rec;
    if (mft->usa_offset == 0 || mft->usa_count < 2) return;
    uint16_t *usa = (uint16_t *)(rec + mft->usa_offset);
    uint16_t  seq = usa[0];
    uint32_t  n   = mft->usa_count - 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = (i + 1) * g_bytes_per_sec - 2;
        if (off + 2 > rec_size) break;
        uint16_t *p = (uint16_t *)(rec + off);
        if (*p != seq) { /* チェックサム不一致でも続行 */ }
        *p = usa[i + 1];
    }
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
        if (pos + lbytes + obytes > max_len) break;

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
 *  MFT レコードから「最適なファイル名」と「親 MFT#」を抽出する
 *  (Pass 1 / Pass 2 共通のヘルパー)
 * ============================================================ */
typedef struct {
    char     name[MAX_NAME_UTF8];
    uint64_t parent_mft_no;
    uint32_t file_attrs;
    uint8_t  ns;
} BestName;

static int extract_best_name(const uint8_t *rec_buf, BestName *out) {
    const MFT_RECORD *mft = (const MFT_RECORD *)rec_buf;

    typedef struct {
        char     name[MAX_NAME_UTF8];
        uint64_t parent_mft_no;
        uint32_t file_attrs;
        uint8_t  ns;
    } FnEntry;
    FnEntry fn[MAX_FN_ENTRIES];
    int fn_count = 0;

    uint32_t off = mft->attrs_offset;
    while (off + 8 <= g_mft_record_size) {
        uint32_t attr_type = *(uint32_t *)(rec_buf + off);
        uint32_t attr_len  = *(uint32_t *)(rec_buf + off + 4);
        if (attr_type == ATTR_END) break;
        if (attr_len  == 0)        break;
        if (off + attr_len > g_mft_record_size) break;

        if (attr_type == ATTR_FILE_NAME) {
            const ATTR_RES *ar = (const ATTR_RES *)(rec_buf + off);
            if (!ar->non_resident &&
                ar->value_length >= (uint32_t)sizeof(FILE_NAME_ATTR) &&
                ar->value_offset + ar->value_length <= attr_len) {

                const uint8_t      *val = rec_buf + off + ar->value_offset;
                const FILE_NAME_ATTR *fna = (const FILE_NAME_ATTR *)val;
                uint32_t nbytes = fna->name_length * 2u;

                if (fn_count < MAX_FN_ENTRIES &&
                    sizeof(FILE_NAME_ATTR) + nbytes <= ar->value_length) {
                    utf16le_to_utf8(val + sizeof(FILE_NAME_ATTR), nbytes,
                                    fn[fn_count].name, MAX_NAME_UTF8);
                    /* parent_ref の下位 48bit が MFT# */
                    fn[fn_count].parent_mft_no = fna->parent_ref & 0x0000FFFFFFFFFFFFULL;
                    fn[fn_count].file_attrs    = fna->file_attrs;
                    fn[fn_count].ns            = fna->name_namespace;
                    fn_count++;
                }
            }
        }
        off += attr_len;
    }

    if (fn_count == 0) return 0; /* エントリなし */

    /* Win32 > Win32&DOS > POSIX > DOS の優先順 */
    int best = 0;
    for (int i = 0; i < fn_count; i++) {
        if (fn[i].ns == FILE_NAME_WIN32 || fn[i].ns == FILE_NAME_WIN32DOS) {
            best = i; break;
        }
    }
    /* Win32 が見つからなければ POSIX を探す */
    if (fn[best].ns == FILE_NAME_DOS) {
        for (int i = 0; i < fn_count; i++) {
            if (fn[i].ns == FILE_NAME_POSIX) { best = i; break; }
        }
    }

    memcpy(out, &fn[best], sizeof(BestName));
    return 1;
}

/* ============================================================
 *  ディレクトリテーブル操作
 * ============================================================ */
static void dir_table_ensure_index(uint64_t mft_no) {
    if (mft_no >= g_mft_index_cap) {
        uint64_t new_cap = mft_no + 1024;
        g_mft_index = realloc(g_mft_index, new_cap * sizeof(uint64_t));
        if (!g_mft_index) { perror("realloc mft_index"); exit(1); }
        for (uint64_t i = g_mft_index_cap; i < new_cap; i++)
            g_mft_index[i] = UINT64_MAX;
        g_mft_index_cap = new_cap;
    }
}

static void dir_table_add(uint64_t mft_no, uint64_t parent_mft_no,
                           const char *name) {
    dir_table_ensure_index(mft_no);

    /* 既存エントリを更新 (同じ MFT# が複数回来た場合) */
    if (g_mft_index[mft_no] != UINT64_MAX) {
        uint64_t idx = g_mft_index[mft_no];
        g_dir_table[idx].parent_mft_no = parent_mft_no;
        snprintf(g_dir_table[idx].name, MAX_NAME_UTF8, "%s", name);
        return;
    }

    /* 新規追加 */
    if (g_dir_count >= g_dir_cap) {
        g_dir_cap = g_dir_cap ? g_dir_cap * 2 : 1024;
        g_dir_table = realloc(g_dir_table, g_dir_cap * sizeof(DirEntry));
        if (!g_dir_table) { perror("realloc dir_table"); exit(1); }
    }

    uint64_t idx = g_dir_count++;
    g_dir_table[idx].mft_no        = mft_no;
    g_dir_table[idx].parent_mft_no = parent_mft_no;
    snprintf(g_dir_table[idx].name, MAX_NAME_UTF8, "%s", name);
    g_dir_table[idx].valid         = 1;
    g_mft_index[mft_no]            = idx;
}

/*
 * MFT# からフルパスを組み立てる。
 *
 * leaf_name は既に呼び出し側で確定しているので、
 * ループでは「cur の親」から上へ辿り、
 * 「cur の親の名前」= cur の親エントリの name を積む。
 *
 *   例: longtext.txt (MFT#46)
 *     parts[0] = "longtext.txt"   ← leaf として事前に積む
 *     cur=46 → parent=38(subfolder11)
 *       cur=38 → push "subfolder11", parent=36(folder1)
 *       cur=36 → push "folder1",     parent=5(ROOT) → 終了
 *     結果: \ + folder1 + \ + subfolder11 + \ + longtext.txt
 */
static void build_fullpath(uint64_t mft_no, const char *leaf_name,
                            char *out, size_t out_size) {
#define MAX_DEPTH 64
    const char *parts[MAX_DEPTH];
    int depth = 0;

    /* leaf (ファイル名自身) を最初に積む */
    parts[depth++] = leaf_name;

    /* cur の親を辿り、cur 自身の名前を積んでいく */
    uint64_t cur = mft_no;
    int loop_guard = 0;

    while (loop_guard++ < 256) {
        /* cur のエントリを引く */
        if (cur >= g_mft_index_cap || g_mft_index[cur] == UINT64_MAX) {
            if (depth < MAX_DEPTH) parts[depth++] = "<orphan>";
            break;
        }

        uint64_t idx    = g_mft_index[cur];
        uint64_t parent = g_dir_table[idx].parent_mft_no;

        /* ルート到達 or 自己参照 → 終了 */
        if (parent == MFT_ROOT || parent == cur || cur == MFT_ROOT) break;

        /* cur の親エントリを引いて、その名前を積む */
        if (parent >= g_mft_index_cap || g_mft_index[parent] == UINT64_MAX) {
            if (depth < MAX_DEPTH) parts[depth++] = "<orphan>";
            break;
        }

        uint64_t pidx = g_mft_index[parent];
        if (parent != MFT_ROOT && depth < MAX_DEPTH)
            parts[depth++] = g_dir_table[pidx].name;

        cur = parent;
    }

    if (loop_guard >= 256 && depth < MAX_DEPTH)
        parts[depth++] = "<loop>";

    /* スタックを逆順に結合: \ component \ component \ ... leaf */
    out[0] = '\0';
    size_t pos = 0;

    if (pos + 1 < out_size) { out[pos++] = '\\'; out[pos] = '\0'; }

    for (int i = depth - 1; i >= 0; i--) {
        size_t plen = strlen(parts[i]);
        if (pos + plen < out_size) {
            memcpy(out + pos, parts[i], plen);
            pos += plen;
            out[pos] = '\0';
        }
        if (i > 0 && pos + 1 < out_size) {
            out[pos++] = '\\';
            out[pos]   = '\0';
        }
    }
#undef MAX_DEPTH
}

/* ============================================================
 *  MFT 全走査の共通ランリスト列挙
 *  コールバック関数ポインタで Pass 1 / Pass 2 を切り替える
 * ============================================================ */
typedef void (*RecordCallback)(const uint8_t *rec_buf, void *userdata);

static void scan_mft(const uint8_t *mft0_buf,
                      RecordCallback cb, void *userdata,
                      uint64_t *out_total) {
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

        if (atype == ATTR_DATA) {
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
        fprintf(stderr, "[警告] MFT#0 の $DATA ランリストが見つかりません\n");
        return;
    }

    RunEntry runs[4096];
    int num_runs = parse_runlist(runlist_ptr, runlist_max_len, runs, 4096);
    if (num_runs == 0) {
        fprintf(stderr, "[警告] ランリストが空です\n");
        return;
    }

    uint64_t total_records = data_size / g_mft_record_size;
    if (out_total) *out_total = total_records;

    uint8_t *rec_buf = malloc(g_mft_record_size);
    if (!rec_buf) { perror("malloc"); return; }

    uint64_t rec_idx = 0;

    for (int ri = 0; ri < num_runs && rec_idx < total_records; ri++) {
        if (runs[ri].lcn == (uint64_t)-1) {
            rec_idx += runs[ri].run_len * (g_bytes_per_clus / g_mft_record_size);
            continue;
        }

        uint64_t run_byte_off = runs[ri].lcn * g_bytes_per_clus;
        uint64_t run_byte_len = runs[ri].run_len * g_bytes_per_clus;
        uint64_t pos = 0;

        while (pos + g_mft_record_size <= run_byte_len &&
               rec_idx < total_records) {
            off_t phys = (off_t)(run_byte_off + pos);
            ssize_t r = full_pread(g_dev_fd, rec_buf, g_mft_record_size, phys);
            if (r == (ssize_t)g_mft_record_size) {
                MFT_RECORD *mft = (MFT_RECORD *)rec_buf;
                if (mft->magic == MFT_RECORD_MAGIC) {
                    apply_fixup(rec_buf, g_mft_record_size);
                    cb(rec_buf, userdata);
                }
            }
            pos      += g_mft_record_size;
            rec_idx++;
        }
    }

    free(rec_buf);
}

/* ============================================================
 *  Pass 1 コールバック: ディレクトリテーブル構築
 * ============================================================ */
static void pass1_cb(const uint8_t *rec_buf, void *userdata) {
    (void)userdata;
    const MFT_RECORD *mft = (const MFT_RECORD *)rec_buf;

    BestName bn;
    if (!extract_best_name(rec_buf, &bn)) return;

    dir_table_add((uint64_t)mft->mft_record_number,
                  bn.parent_mft_no,
                  bn.name);
}

/* ============================================================
 *  Pass 2 コールバック: CSV 出力
 * ============================================================ */
typedef struct {
    FILE    *csv_fp;
    uint64_t in_use;
    uint64_t deleted;
    uint64_t total;
} Pass2Data;

static void pass2_cb(const uint8_t *rec_buf, void *userdata) {
    Pass2Data        *d   = (Pass2Data *)userdata;
    const MFT_RECORD *mft = (const MFT_RECORD *)rec_buf;

    const char *status = (mft->flags & MFT_RECORD_IN_USE) ? "IN_USE" : "DELETED";

    /* タイムスタンプ */
    uint64_t si_created = 0, si_modified = 0, si_accessed = 0;

    /* $FILE_NAME エントリ */
    typedef struct {
        char     name[MAX_NAME_UTF8];
        uint64_t parent_mft_no;
        uint16_t parent_seq_no;   /* parent_ref の上位 16bit = 親の SeqNo */
        uint32_t file_attrs;
        uint8_t  ns;
    } FnEntry;
    FnEntry fn[MAX_FN_ENTRIES];
    int fn_count = 0;

    /* ADS: 名前と各ストリームのサイズ */
    typedef struct {
        char     name[MAX_NAME_UTF8];
        uint64_t size;
    } AdsEntry;
    AdsEntry ads[MAX_ADS_ENTRIES];
    int      ads_count = 0;

    /* 無名 $DATA のサイズ (通常ファイルのファイルサイズ) */
    uint64_t main_data_size = 0;

    /* reparse */
    int has_reparse = 0;

    /* 属性走査 */
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

        if (atype == ATTR_REPARSE_POINT) has_reparse = 1;

        if (!ar->non_resident) {
            if (ar->value_offset + ar->value_length > alen) goto next_attr;
            const uint8_t *val = rec_buf + off + ar->value_offset;

            if (atype == ATTR_STANDARD_INFORMATION && ar->value_length >= 32) {
                const STD_INFO *si = (const STD_INFO *)val;
                si_created  = si->created;
                si_modified = si->modified;
                si_accessed = si->accessed;
            }

            if (atype == ATTR_FILE_NAME &&
                ar->value_length >= (uint32_t)sizeof(FILE_NAME_ATTR)) {
                const FILE_NAME_ATTR *fna = (const FILE_NAME_ATTR *)val;
                uint32_t nbytes = fna->name_length * 2u;
                if (fn_count < MAX_FN_ENTRIES &&
                    sizeof(FILE_NAME_ATTR) + nbytes <= ar->value_length) {
                    utf16le_to_utf8(val + sizeof(FILE_NAME_ATTR), nbytes,
                                    fn[fn_count].name, MAX_NAME_UTF8);
                    fn[fn_count].parent_mft_no =
                        fna->parent_ref & 0x0000FFFFFFFFFFFFULL;
                    fn[fn_count].parent_seq_no =
                        (uint16_t)(fna->parent_ref >> 48);
                    fn[fn_count].file_attrs = fna->file_attrs;
                    fn[fn_count].ns         = fna->name_namespace;
                    fn_count++;
                }
            }

            if (atype == ATTR_DATA) {
                if (attr_name[0] == '\0') {
                    /* 無名 $DATA = 通常ファイルのデータ (常駐) */
                    main_data_size = ar->value_length;
                } else {
                    /* 名前付き $DATA = ADS (常駐) */
                    if (ads_count < MAX_ADS_ENTRIES) {
                        snprintf(ads[ads_count].name, MAX_NAME_UTF8, "%s", attr_name);
                        ads[ads_count].size = ar->value_length;
                        ads_count++;
                    }
                }
            }
        } else {
            /* 非常駐属性 */
            const ATTR_NONRES *anr = (const ATTR_NONRES *)(rec_buf + off);
            if (atype == ATTR_REPARSE_POINT) has_reparse = 1;
            if (atype == ATTR_DATA) {
                if (attr_name[0] == '\0') {
                    /* 無名 $DATA (非常駐) */
                    main_data_size = anr->data_size;
                } else {
                    /* 名前付き $DATA = ADS (非常駐) */
                    if (ads_count < MAX_ADS_ENTRIES) {
                        snprintf(ads[ads_count].name, MAX_NAME_UTF8, "%s", attr_name);
                        ads[ads_count].size = anr->data_size;
                        ads_count++;
                    }
                }
            }
        }

    next_attr:
        off += alen;
    }

    if (fn_count == 0) return;

    /* 最適 $FILE_NAME を選択 */
    int best = 0;
    for (int i = 0; i < fn_count; i++) {
        if (fn[i].ns == FILE_NAME_WIN32 || fn[i].ns == FILE_NAME_WIN32DOS) {
            best = i; break;
        }
    }
    if (fn[best].ns == FILE_NAME_DOS) {
        for (int i = 0; i < fn_count; i++) {
            if (fn[i].ns == FILE_NAME_POSIX) { best = i; break; }
        }
    }

    const char *main_name      = fn[best].name;
    uint32_t    main_attrs     = fn[best].file_attrs;
    uint64_t    parent_mft_no  = fn[best].parent_mft_no;
    uint16_t    parent_seq_no  = fn[best].parent_seq_no;
    uint64_t    mft_rec_no     = mft->mft_record_number;
    uint16_t    mft_seq_no     = mft->seq_number;

    /* type 判定 */
    int is_dir = (mft->flags & MFT_RECORD_IS_DIR) || (main_attrs & FA_DIRECTORY);
    const char *type;
    if      (has_reparse && is_dir) type = "JUNCTION";
    else if (has_reparse)           type = "SYMLINK";
    else if (is_dir)                type = "DIR";
    else                            type = "FILE";

    /* フルパス構築: 自分自身の MFT# を起点に親チェーンを辿る */
    char fullpath[MAX_PATH_UTF8];
    build_fullpath(mft_rec_no, main_name, fullpath, sizeof(fullpath));

    /* タイムスタンプ文字列 */
    char ts_mod[40], ts_acc[40], ts_cre[40];
    ntfs_time_to_str(si_modified, ts_mod, sizeof(ts_mod));
    ntfs_time_to_str(si_accessed, ts_acc, sizeof(ts_acc));
    ntfs_time_to_str(si_created,  ts_cre, sizeof(ts_cre));

    /* mft_ref,parent_mft_ref,status,type,modified_tm,accessed_tm,created_tm,filename,fullpath,file_size */
    #define EMIT_ROW(fname, fpath, fsize) do { \
        fprintf(d->csv_fp, "%llu:%u,%llu:%u,%s,%s,", \
                (unsigned long long)mft_rec_no,    (unsigned)mft_seq_no, \
                (unsigned long long)parent_mft_no, (unsigned)parent_seq_no, \
                status, type); \
        csv_write_field(d->csv_fp, ts_mod); fputc(',', d->csv_fp); \
        csv_write_field(d->csv_fp, ts_acc); fputc(',', d->csv_fp); \
        csv_write_field(d->csv_fp, ts_cre); fputc(',', d->csv_fp); \
        csv_write_field(d->csv_fp, fname);  fputc(',', d->csv_fp); \
        csv_write_field(d->csv_fp, fpath);  fputc(',', d->csv_fp); \
        fprintf(d->csv_fp, "%llu\n", (unsigned long long)(fsize)); \
    } while(0)

    /* DIR のサイズは 0、FILE は無名 $DATA のサイズ */
    uint64_t row_size = is_dir ? 0 : main_data_size;
    EMIT_ROW(main_name, fullpath, row_size);

    /* ADS 行 */
    for (int i = 0; i < ads_count; i++) {
        char ads_fname[MAX_NAME_UTF8 * 2 + 2];
        int wr = snprintf(ads_fname, sizeof(ads_fname), "%s:", main_name);
        snprintf(ads_fname + wr, sizeof(ads_fname) - (size_t)wr,
                 "%s", ads[i].name);

        /* ADS のフルパス */
        char ads_fpath[MAX_PATH_UTF8 + MAX_NAME_UTF8 + 2];
        {
            int wr2 = snprintf(ads_fpath, sizeof(ads_fpath), "%s:", fullpath);
            snprintf(ads_fpath + wr2, sizeof(ads_fpath) - (size_t)wr2,
                     "%s", ads[i].name);
        }

        fprintf(d->csv_fp, "%llu:%u,%llu:%u,%s,ADS,",
                (unsigned long long)mft_rec_no,    (unsigned)mft_seq_no,
                (unsigned long long)parent_mft_no, (unsigned)parent_seq_no,
                status);
        csv_write_field(d->csv_fp, ts_mod); fputc(',', d->csv_fp);
        csv_write_field(d->csv_fp, ts_acc); fputc(',', d->csv_fp);
        csv_write_field(d->csv_fp, ts_cre); fputc(',', d->csv_fp);
        csv_write_field(d->csv_fp, ads_fname); fputc(',', d->csv_fp);
        csv_write_field(d->csv_fp, ads_fpath); fputc(',', d->csv_fp);
        fprintf(d->csv_fp, "%llu\n", (unsigned long long)ads[i].size);
    }

    #undef EMIT_ROW

    if (mft->flags & MFT_RECORD_IN_USE) d->in_use++;
    else                                  d->deleted++;
    d->total++;
}

/* ============================================================
 *  main
 * ============================================================ */
int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    if (argc < 2) {
        fprintf(stderr,
            "使い方: %s <デバイス> [出力CSV]\n"
            "  例:  sudo %s /dev/loop0 output.csv\n"
            "       sudo %s /dev/sda1\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *device   = argv[1];
    const char *csv_path = (argc >= 3) ? argv[2] : NULL;

    g_dev_fd = open(device, O_RDONLY);
    if (g_dev_fd < 0) {
        fprintf(stderr, "Error: '%s' を開けません: %s\n",
                device, strerror(errno));
        return 1;
    }

    /* ブートセクタ */
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

    fprintf(stderr, "# Device            : %s\n",  device);
    fprintf(stderr, "# Sector Size       : %u B\n", g_bytes_per_sec);
    fprintf(stderr, "# Cluster Size      : %u B\n", g_bytes_per_clus);
    fprintf(stderr, "# MFT Record Size   : %u B\n", g_mft_record_size);
    fprintf(stderr, "# MFT from LCN      : %llu\n", (unsigned long long)boot.mft_lcn);
    fprintf(stderr, "# MFT from byte     : 0x%llx B\n", (unsigned long long)g_mft_byte_offset);
    fprintf(stderr, "# MFT Mirr from LCN : %llu\n", (unsigned long long)boot.mft_mirror_lcn);
    fprintf(stderr, "# MFT Mirr from byte: 0x%llx B\n", (unsigned long long)(boot.mft_mirror_lcn * g_bytes_per_clus));

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

    /* ── Pass 1: ディレクトリテーブル構築 ── */
    fprintf(stderr, "# Pass 1: ディレクトリテーブル構築中...\n");
    uint64_t total_records = 0;
    scan_mft(mft0_buf, pass1_cb, NULL, &total_records);
    fprintf(stderr, "# 総 MFT レコード数   : %llu\n",
            (unsigned long long)total_records);
    fprintf(stderr, "# ディレクトリエントリ: %llu\n",
            (unsigned long long)g_dir_count);

    /* ── CSV 出力先 ── */
    FILE *csv_fp = stdout;
    if (csv_path) {
        csv_fp = fopen(csv_path, "w");
        if (!csv_fp) {
            fprintf(stderr, "Error: '%s' を開けません: %s\n",
                    csv_path, strerror(errno));
            free(mft0_buf); close(g_dev_fd); return 1;
        }
    }

    /* ヘッダ */
    fprintf(csv_fp,
            "mft_ref,parent_mft_ref,status,type,modified_tm,accessed_tm,created_tm,"
            "filename,fullpath,file_size\n");

    /* ── Pass 2: CSV 出力 ── */
    fprintf(stderr, "# Pass 2: CSV 出力中...\n");
    Pass2Data pd = { csv_fp, 0, 0, 0 };
    scan_mft(mft0_buf, pass2_cb, &pd, NULL);

    if (csv_fp != stdout) fclose(csv_fp);
    free(mft0_buf);
    free(g_dir_table);
    free(g_mft_index);
    close(g_dev_fd);

    fprintf(stderr, "# ─────────────────────────────────\n");
    fprintf(stderr, "# 処理済みレコード: %llu\n",  (unsigned long long)pd.total);
    fprintf(stderr, "# IN_USE          : %llu\n",  (unsigned long long)pd.in_use);
    fprintf(stderr, "# DELETED         : %llu\n",  (unsigned long long)pd.deleted);
    if (csv_path)
        fprintf(stderr, "# 出力ファイル    : %s\n", csv_path);

    return 0;
}
