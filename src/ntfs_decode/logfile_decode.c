/*
 * logfile_decode.c - NTFS $LogFile (LFS) パーサ
 *
 * $LogFile から LFS ログレコードをすべて抽出し CSV に出力する。
 *
 * 解析対象:
 *   Restart Pages (RSTR) … ボリュームパラメータの取得
 *   Log Record Pages (RCRD) … LFS ログレコードの抽出
 *
 * 出力 CSV 列:
 *   LSN, PrevLSN, UndoLSN, TransID, RedoOp, UndoOp,
 *   TargetMFT, TargetAttr, VCN, Filename
 *
 * コンパイル:
 *   gcc -O2 -Wall -o logfile_decode logfile_decode.c   # Linux
 *   cc  -O2 -Wall -o logfile_decode logfile_decode.c   # macOS
 *
 * 実行:
 *   ./logfile_decode logfile.bin > logfile.csv
 *   ./logfile_decode logfile.bin logfile.csv
 *
 * $LogFile 抽出 (The Sleuth Kit):
 *   icat -o <セクタオフセット> image.raw 2 > logfile.bin
 *   (MFT# 2 = $LogFile)
 *
 * フォレンジック上の用途:
 *   - ファイル作成の証明（InitializeFileRecordSegment は名前も MFT# も取れる）
 *   - 属性レベルの操作追跡（作成・削除・改名・属性更新を REDO/UNDO で）
 *   - トランザクション完了の確認（ForgetTransaction。通常操作に
 *     CommitTransaction/PrepareTransaction は出ない点に注意）
 *   - USN ジャーナルとの突合せ（削除・改名の MFT# は $MFT 対象ゆえ
 *     $LogFile 単独では解決しづらく、名前は USN と重ねて確定する）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

/* ============================================================
 *  定数
 * ============================================================ */
#define DEFAULT_PAGE_SIZE    4096u
#define SECTOR_SIZE          512u
#define MAX_LOGFILE_SIZE     (256u * 1024u * 1024u)  /* 256 MB 上限 */
#define MAX_ATTR_TABLE       512

/* ページマジック */
#define MAGIC_RSTR  0x52545352U   /* "RSTR" リトルエンディアン */
#define MAGIC_RCRD  0x44524352U   /* "RCRD" */
#define MAGIC_FILE  0x454C4946U   /* "FILE" (MFT レコード) */

/* LFS レコード種別 */
#define LFS_RECORD_CLIENT   1     /* 通常の NTFS ログレコード */
#define LFS_RECORD_RESTART  2     /* クライアント再起動レコード */

/* NTFS 属性型 */
#define ATTR_FILE_NAME      0x30U
#define ATTR_END            0xFFFFFFFFU

/* ============================================================
 *  オンディスク構造体
 * ============================================================ */
#pragma pack(push, 1)

/* Restart Page ヘッダ ("RSTR") */
typedef struct {
    uint32_t magic;               /* 0x52545352 */
    uint16_t usa_offset;          /* Update Sequence Array オフセット */
    uint16_t usa_count;           /* USA エントリ数 */
    uint64_t chkdsk_lsn;          /* chkdsk が最後に書いた LSN */
    uint32_t system_page_size;    /* システムページサイズ（通常 4096） */
    uint32_t log_page_size;       /* ログページサイズ（通常 4096） */
    uint16_t restart_area_offset; /* Restart Area へのオフセット */
    int16_t  minor_version;       /* 通常 0 */
    int16_t  major_version;       /* 1 または 2 */
    uint16_t reserved;
} LFS_RESTART_PAGE_HEADER;

/* Restart Area（Restart Page ヘッダの後に続く） */
typedef struct {
    uint64_t current_lsn;             /* 現在の最新 LSN */
    uint16_t log_clients;             /* クライアント数（通常 1 = NTFS） */
    uint16_t client_free_list;        /* 空きクライアントリスト */
    uint16_t client_in_use_list;      /* 使用中クライアントリスト */
    uint16_t flags;
    uint32_t seq_number_bits;         /* LSN のシーケンスビット数 */
    uint16_t restart_area_length;
    uint16_t client_array_offset;     /* クライアント配列へのオフセット */
    uint64_t file_size;               /* $LogFile の論理サイズ */
    uint32_t last_lsn_data_length;
    uint16_t log_record_header_length;/* ログレコードヘッダ長（通常 0x30） */
    uint16_t log_page_data_offset;    /* ページ内データ開始オフセット */
} LFS_RESTART_AREA;

/* クライアントレコード（Restart Area 内のクライアント情報） */
typedef struct {
    uint64_t oldest_lsn;           /* 最も古い有効な LSN */
    uint64_t client_restart_lsn;   /* クライアントの最新 restart LSN */
    uint16_t prev_client;
    uint16_t next_client;
    uint16_t seq_number;
    uint8_t  pad[6];
    uint32_t client_name_length;   /* クライアント名のバイト数 */
    uint16_t client_name[64];      /* UTF-16LE, 通常 "NTFS" */
} LFS_CLIENT_RECORD;

/* Log Record Page ヘッダ ("RCRD") */
typedef struct {
    uint32_t magic;               /* 0x44524352 */
    uint16_t usa_offset;
    uint16_t usa_count;
    uint64_t last_lsn;            /* このページ最後の完全な LSN */
    uint32_t flags;
    uint16_t page_count;          /* 複数ページにまたがる場合の総ページ数 */
    uint16_t page_position;       /* このページの位置（1 始まり） */
    uint16_t next_record_offset;  /* 次の空き領域の先頭オフセット */
    uint8_t  pad[6];
    uint64_t last_end_lsn;        /* ページ末尾の LSN */
} LFS_RECORD_PAGE_HEADER;

/* LFS ログレコードヘッダ（48 バイト = 0x30） */
typedef struct {
    uint64_t this_lsn;                /* このレコードの LSN */
    uint64_t client_prev_lsn;        /* 同一クライアントの前レコード LSN */
    uint64_t client_undo_next_lsn;   /* UNDO チェーンの次の LSN */
    uint32_t client_data_length;     /* クライアントデータ（NTFS_LOG_RECORD）のバイト数 */
    uint16_t client_id_seq;          /* クライアント ID（シーケンス番号） */
    uint16_t client_id_index;        /* クライアント ID（インデックス） */
    uint32_t record_type;            /* 1=client_record, 2=client_restart */
    uint32_t transaction_id;         /* トランザクション ID */
    uint16_t flags;                  /* 0x0001 = 複数ページにまたがる */
    uint8_t  pad[6];
} LFS_LOG_RECORD_HEADER;             /* sizeof = 48 = 0x30 */

/* NTFS ログレコード（LFS ログレコードヘッダの直後から始まるクライアントデータ） */
typedef struct {
    uint16_t redo_op;            /* REDO 操作コード */
    uint16_t undo_op;            /* UNDO 操作コード */
    uint16_t redo_offset;        /* REDO データの先頭オフセット（NTFS_LOG_RECORD 先頭から） */
    uint16_t redo_length;        /* REDO データのバイト数 */
    uint16_t undo_offset;        /* UNDO データの先頭オフセット */
    uint16_t undo_length;        /* UNDO データのバイト数 */
    uint16_t target_attribute;   /* Open Attribute Table へのインデックス */
    uint16_t lcns_to_follow;     /* 後続する LCN エントリ数 */
    uint16_t record_offset;      /* MFT レコード内オフセット */
    uint16_t attribute_offset;   /* 属性値内オフセット */
    uint16_t cluster_index;      /* クラスタ内インデックス */
    uint16_t reserved;
    uint64_t target_vcn;         /* 対象クラスタの VCN */
    /* 続いて lcns_to_follow × 8 バイトの LCN 配列 */
    /* 続いて redo_offset から redo データ */
    /* 続いて undo_offset から undo データ */
} NTFS_LOG_RECORD;               /* sizeof = 32 = 0x20（LCN 配列は含まない） */

/* $FILE_NAME 属性値ヘッダ */
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
    uint8_t  name_length;       /* 文字数（バイト数ではない） */
    uint8_t  name_namespace;
} FILE_NAME_ATTR;

/* 常駐属性ヘッダ（CreateAttribute の redo データで使用） */
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

/* MFT レコードヘッダ（InitializeFileRecordSegment の redo データで使用） */
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

#pragma pack(pop)

/* ============================================================
 *  Open Attribute Table
 *
 *  OpenNonresidentAttribute (op=0x001C) レコードが来るたびに
 *  target_attribute → MFT# のマッピングを記録する。
 *  他のレコードはこの表を使って target_attribute からMFT#を解決する。
 * ============================================================ */
typedef struct {
    uint16_t attr_idx;   /* target_attribute の値 */
    uint64_t mft_no;     /* 対応する MFT# */
    uint32_t attr_type;  /* 属性型 */
    int      valid;
} AttrEntry;

static AttrEntry g_attr_table[MAX_ATTR_TABLE];
static int       g_attr_count = 0;

static uint64_t attr_lookup(uint16_t idx) {
    for (int i = 0; i < g_attr_count; i++)
        if (g_attr_table[i].valid && g_attr_table[i].attr_idx == idx)
            return g_attr_table[i].mft_no;
    return (uint64_t)-1;
}

static void attr_register(uint16_t idx, uint64_t mft_no, uint32_t atype) {
    for (int i = 0; i < g_attr_count; i++) {
        if (g_attr_table[i].attr_idx == idx) {
            g_attr_table[i].mft_no   = mft_no;
            g_attr_table[i].attr_type = atype;
            g_attr_table[i].valid    = 1;
            return;
        }
    }
    if (g_attr_count < MAX_ATTR_TABLE) {
        int i = g_attr_count++;
        g_attr_table[i].attr_idx  = idx;
        g_attr_table[i].mft_no   = mft_no;
        g_attr_table[i].attr_type = atype;
        g_attr_table[i].valid    = 1;
    }
}

/* ============================================================
 *  グローバル（ログパラメータ）
 *  Restart Page を解析して確定する
 * ============================================================ */
static uint32_t g_page_size         = DEFAULT_PAGE_SIZE;
static uint16_t g_log_rec_hdr_len   = 0x30;   /* 48 バイト */
static uint16_t g_log_page_data_off = 0x28;   /* デフォルト */

/* ============================================================
 *  NTFS 操作コード → 文字列
 * ============================================================ */
static const char *ntfs_op_name(uint16_t op) {
    switch (op) {
        case 0x0000: return "Noop";
        case 0x0001: return "CompensationLogRecord";
        case 0x0002: return "InitializeFileRecordSegment";
        case 0x0003: return "DeallocateFileRecordSegment";
        case 0x0004: return "WriteEndOfFileRecordSegment";
        case 0x0005: return "CreateAttribute";
        case 0x0006: return "DeleteAttribute";
        case 0x0007: return "UpdateResidentValue";
        case 0x0008: return "UpdateNonresidentValue";
        case 0x0009: return "UpdateMappingPairs";
        case 0x000A: return "DeleteDirtyClusters";
        case 0x000B: return "SetNewAttributeSizes";
        case 0x000C: return "AddIndexEntryRoot";
        case 0x000D: return "DeleteIndexEntryRoot";
        case 0x000E: return "AddIndexEntryAllocation";
        case 0x000F: return "DeleteIndexEntryAllocation";
        case 0x0010: return "WriteEndOfIndexBuffer";
        case 0x0011: return "SetIndexEntryVcnRoot";
        case 0x0012: return "SetIndexEntryVcnAllocation";
        case 0x0013: return "UpdateFileNameRoot";
        case 0x0014: return "UpdateFileNameAllocation";
        case 0x0015: return "SetBitsInNonresidentBitMap";
        case 0x0016: return "ClearBitsInNonresidentBitMap";
        case 0x0017: return "HotFix";
        case 0x0018: return "EndTopLevelAction";
        case 0x0019: return "PrepareTransaction";
        case 0x001A: return "CommitTransaction";
        case 0x001B: return "ForgetTransaction";
        case 0x001C: return "OpenNonresidentAttribute";
        case 0x001D: return "OpenAttributeTableDump";
        case 0x001E: return "AttributeNamesDump";
        case 0x001F: return "DirtyPageTableDump";
        case 0x0020: return "TransactionTableDump";
        case 0x0021: return "UpdateRecordDataRoot";
        case 0x0022: return "UpdateRecordDataAllocation";
        default:     return "Unknown";
    }
}

/* ============================================================
 *  UTF-16LE → UTF-8（BMP のみ、iconv 不要）
 * ============================================================ */
static void u16le_to_u8(const uint8_t *src, size_t src_bytes,
                         char *dst, size_t dst_size) {
    size_t di = 0;
    for (size_t i = 0; i + 1 < src_bytes && di + 4 < dst_size; i += 2) {
        uint16_t w = src[i] | ((uint16_t)src[i + 1] << 8);
        if (w == 0) break;
        if      (w < 0x80)  { dst[di++] = (char)w; }
        else if (w < 0x800) { dst[di++] = (char)(0xC0 | (w >> 6));
                               dst[di++] = (char)(0x80 | (w & 0x3F)); }
        else                { dst[di++] = (char)(0xE0 | (w >> 12));
                               dst[di++] = (char)(0x80 | ((w >> 6) & 0x3F));
                               dst[di++] = (char)(0x80 | (w & 0x3F)); }
    }
    dst[di] = '\0';
}

/* ============================================================
 *  USA（Fix-up）適用
 * ============================================================ */
static void apply_usa(uint8_t *page, uint32_t page_sz,
                      uint16_t usa_off, uint16_t usa_cnt) {
    if (usa_off == 0 || usa_cnt < 2) return;
    uint16_t *usa = (uint16_t *)(page + usa_off);
    uint32_t  n   = (uint32_t)(usa_cnt - 1);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = (i + 1) * SECTOR_SIZE - 2;
        if (off + 2 > page_sz) break;
        *(uint16_t *)(page + off) = usa[i + 1];
    }
}

/* ============================================================
 *  Restart Page の解析
 *  g_page_size / g_log_rec_hdr_len / g_log_page_data_off を確定する
 * ============================================================ */
static void parse_restart_page(const uint8_t *page) {
    const LFS_RESTART_PAGE_HEADER *rp = (const LFS_RESTART_PAGE_HEADER *)page;
    if (rp->magic != MAGIC_RSTR) return;

    fprintf(stderr, "# RSTR: major=%d minor=%d sys_page=%u log_page=%u\n",
            rp->major_version, rp->minor_version,
            rp->system_page_size, rp->log_page_size);

    if (rp->log_page_size >= 512 && rp->log_page_size <= 65536)
        g_page_size = rp->log_page_size;

    /* restart_area_offset が示す Restart Area */
    uint16_t ra_off = rp->restart_area_offset;
    if ((uint32_t)ra_off + (uint32_t)sizeof(LFS_RESTART_AREA) > DEFAULT_PAGE_SIZE) return;
    const LFS_RESTART_AREA *ra = (const LFS_RESTART_AREA *)(page + ra_off);

    fprintf(stderr, "# RSTR: current_lsn=%" PRIu64 " file_size=%" PRIu64 "\n",
            ra->current_lsn, ra->file_size);

    if (ra->log_record_header_length >= 0x30)
        g_log_rec_hdr_len = ra->log_record_header_length;
    if (ra->log_page_data_offset >= 0x28)
        g_log_page_data_off = ra->log_page_data_offset;

    fprintf(stderr, "# RSTR: rec_hdr_len=0x%x page_data_off=0x%x\n",
            g_log_rec_hdr_len, g_log_page_data_off);

    /* クライアント名の表示（通常 "NTFS"） */
    if (ra->client_in_use_list != 0xFFFF &&
        ra_off + ra->client_array_offset +
        (uint32_t)(ra->client_in_use_list + 1) * (uint32_t)sizeof(LFS_CLIENT_RECORD)
        <= DEFAULT_PAGE_SIZE) {
        const LFS_CLIENT_RECORD *cr = (const LFS_CLIENT_RECORD *)
            (page + ra_off + ra->client_array_offset +
             ra->client_in_use_list * (uint16_t)sizeof(LFS_CLIENT_RECORD));
        char cname[128] = "";
        u16le_to_u8((const uint8_t *)cr->client_name,
                    cr->client_name_length, cname, sizeof(cname));
        fprintf(stderr, "# RSTR: client='%s' oldest_lsn=%" PRIu64
                " restart_lsn=%" PRIu64 "\n",
                cname, cr->oldest_lsn, cr->client_restart_lsn);
    }
}

/* ============================================================
 *  OpenNonresidentAttribute の処理
 *
 *  redo データに含まれる MFT 参照と属性型を読み取り
 *  open attribute table に登録する。
 *
 *  redo データのレイアウト:
 *    offset 0x00: uint64_t file_reference_number  (下位 48bit = MFT#)
 *    offset 0x08: uint32_t attribute_type
 *    offset 0x0C: uint32_t name_length (chars)
 *    offset 0x10: UTF-16LE name (省略可)
 * ============================================================ */
static void process_open_attr(const NTFS_LOG_RECORD *nlr,
                               const uint8_t *cdata, uint32_t clen) {
    if (nlr->redo_offset == 0 || nlr->redo_length < 12) return;
    if ((uint32_t)nlr->redo_offset + 12 > clen) return;

    const uint8_t *rd     = cdata + nlr->redo_offset;
    uint64_t mft_ref      = *(const uint64_t *)(rd);
    uint32_t attr_type    = *(const uint32_t *)(rd + 8);
    uint64_t mft_no       = mft_ref & 0x0000FFFFFFFFFFFFULL;

    /* 妥当性チェック：MFT# が現実的な範囲か */
    if (mft_no < 0x10000000ULL)
        attr_register(nlr->target_attribute, mft_no, attr_type);
}

/* ============================================================
 *  ファイル名の抽出
 *
 *  以下の操作の redo データから $FILE_NAME を探す：
 *
 *  (1) InitializeFileRecordSegment (0x0002)
 *      redo データ = 新しい MFT レコード全体（"FILE" マジックで始まる）
 *      → 属性をスキャンして $FILE_NAME を探す
 *
 *  (2) CreateAttribute (0x0005)
 *      redo データ = 属性ヘッダ（ATTR_RES）+ 属性値
 *      → 属性型が 0x30（$FILE_NAME）なら値を解析
 *
 *  (3) UpdateResidentValue (0x0007)
 *      redo データ = 属性値だけ（ヘッダなし）
 *      → FILE_NAME_ATTR として解析を試みる（ヒューリスティック）
 * ============================================================ */
static void extract_filename(uint16_t redo_op,
                              const uint8_t *rd, uint32_t rd_len,
                              char *out, size_t out_size) {
    out[0] = '\0';
    if (rd_len < 4) return;

    /* ── (1) InitializeFileRecordSegment ── */
    if (redo_op == 0x0002) {
        if (rd_len < sizeof(MFT_RECORD)) return;
        const MFT_RECORD *mft = (const MFT_RECORD *)rd;
        if (mft->magic != MAGIC_FILE) return;
        uint32_t off = mft->attrs_offset;
        while (off + 8 <= rd_len) {
            uint32_t atype = *(const uint32_t *)(rd + off);
            uint32_t alen  = *(const uint32_t *)(rd + off + 4);
            if (atype == ATTR_END || alen == 0) break;
            if (off + alen > rd_len) break;
            if (atype == ATTR_FILE_NAME && alen >= sizeof(ATTR_RES)) {
                const ATTR_RES *ar = (const ATTR_RES *)(rd + off);
                if (!ar->non_resident &&
                    (uint32_t)ar->value_offset + ar->value_length <= alen &&
                    ar->value_length >= (uint32_t)sizeof(FILE_NAME_ATTR)) {
                    const uint8_t *val = rd + off + ar->value_offset;
                    const FILE_NAME_ATTR *fna = (const FILE_NAME_ATTR *)val;
                    uint32_t nb = (uint32_t)fna->name_length * 2u;
                    if (sizeof(FILE_NAME_ATTR) + nb <= ar->value_length) {
                        u16le_to_u8(val + sizeof(FILE_NAME_ATTR), nb, out, out_size);
                        return;
                    }
                }
            }
            off += alen;
        }
        return;
    }

    /* ── (2) CreateAttribute ── */
    if (redo_op == 0x0005 && rd_len >= sizeof(ATTR_RES)) {
        const ATTR_RES *ar = (const ATTR_RES *)rd;
        if (ar->type == ATTR_FILE_NAME && !ar->non_resident &&
            (uint32_t)ar->value_offset + ar->value_length <= rd_len &&
            ar->value_length >= (uint32_t)sizeof(FILE_NAME_ATTR)) {
            const uint8_t *val = rd + ar->value_offset;
            const FILE_NAME_ATTR *fna = (const FILE_NAME_ATTR *)val;
            uint32_t nb = (uint32_t)fna->name_length * 2u;
            if (sizeof(FILE_NAME_ATTR) + nb <= ar->value_length) {
                u16le_to_u8(val + sizeof(FILE_NAME_ATTR), nb, out, out_size);
                return;
            }
        }
    }

    /* ── (3) UpdateResidentValue（ヒューリスティック） ── */
    if (redo_op == 0x0007 && rd_len >= sizeof(FILE_NAME_ATTR)) {
        const FILE_NAME_ATTR *fna = (const FILE_NAME_ATTR *)rd;
        uint64_t par = fna->parent_ref & 0x0000FFFFFFFFFFFFULL;
        /* parent MFT# が妥当 (1〜10M) かつ name_length が非ゼロ */
        if (par >= 1 && par < 0x10000000ULL &&
            fna->name_length > 0) {
            uint32_t nb = (uint32_t)fna->name_length * 2u;
            if (sizeof(FILE_NAME_ATTR) + nb <= rd_len) {
                u16le_to_u8(rd + sizeof(FILE_NAME_ATTR), nb, out, out_size);
                if (out[0] >= 0x20) return;  /* 印字可能文字で始まれば採用 */
            }
        }
        out[0] = '\0';
    }
}

/* ============================================================
 *  RCRD ページ 1 枚の処理
 * ============================================================ */
static uint64_t process_rcrd_page(const uint8_t *page, uint32_t page_sz,
                                   FILE *out_fp) {
    const LFS_RECORD_PAGE_HEADER *ph = (const LFS_RECORD_PAGE_HEADER *)page;
    if (ph->magic != MAGIC_RCRD) return 0;
    if (ph->last_lsn == 0) return 0;   /* 未使用ページ */

    /* 有効データ末尾 */
    uint32_t end_off = ph->next_record_offset;
    if (end_off < g_log_page_data_off || end_off > page_sz)
        end_off = page_sz;

    uint64_t count = 0;
    uint32_t off   = g_log_page_data_off;

    while (off + g_log_rec_hdr_len <= end_off) {
        const LFS_LOG_RECORD_HEADER *lh =
            (const LFS_LOG_RECORD_HEADER *)(page + off);

        if (lh->this_lsn == 0) break;   /* レコードなし */

        /* レコード全体サイズ（8 バイトアライン） */
        uint32_t rec_sz = ((uint32_t)g_log_rec_hdr_len +
                           lh->client_data_length + 7u) & ~7u;
        if (rec_sz < g_log_rec_hdr_len) break;   /* 安全弁 */
        if (off + rec_sz > page_sz) break;

        /* LFS_RECORD_CLIENT (type=1) のみ処理 */
        if (lh->record_type == LFS_RECORD_CLIENT &&
            lh->client_data_length >= sizeof(NTFS_LOG_RECORD)) {

            const uint8_t *cdata  = page + off + g_log_rec_hdr_len;
            const NTFS_LOG_RECORD *nlr = (const NTFS_LOG_RECORD *)cdata;
            uint32_t               clen = lh->client_data_length;

            /* ── Open Attribute Table の更新 ── */
            if (nlr->redo_op == 0x001C)
                process_open_attr(nlr, cdata, clen);

            /* ── MFT# の解決 ── */
            /* 優先: InitializeFileRecordSegment の redo データから直接取得 */
            uint64_t mft_no = (uint64_t)-1;
            if (nlr->redo_op == 0x0002 &&
                nlr->redo_offset != 0 &&
                (uint32_t)nlr->redo_offset + sizeof(MFT_RECORD) <= clen) {
                const MFT_RECORD *mftrec =
                    (const MFT_RECORD *)(cdata + nlr->redo_offset);
                if (mftrec->magic == MAGIC_FILE)
                    mft_no = mftrec->mft_record_number;
            }
            /* 次点: open attribute table による解決 */
            if (mft_no == (uint64_t)-1)
                mft_no = attr_lookup(nlr->target_attribute);

            /* ── ファイル名の抽出 ── */
            char fname[512] = "";
            if (nlr->redo_length > 0 &&
                nlr->redo_offset != 0 &&
                (uint32_t)nlr->redo_offset + nlr->redo_length <= clen) {
                extract_filename(nlr->redo_op,
                                 cdata + nlr->redo_offset, nlr->redo_length,
                                 fname, sizeof(fname));
            }

            /* ── CSV 出力 ──
             * LSN, PrevLSN, UndoLSN, TransID,
             * RedoOp, UndoOp, TargetMFT, TargetAttr, VCN, Filename
             */
            fprintf(out_fp,
                    "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%s,%s,",
                    lh->this_lsn,
                    lh->client_prev_lsn,
                    lh->client_undo_next_lsn,
                    lh->transaction_id,
                    ntfs_op_name(nlr->redo_op),
                    ntfs_op_name(nlr->undo_op));

            if (mft_no != (uint64_t)-1)
                fprintf(out_fp, "%" PRIu64, mft_no);
            else
                fputc('-', out_fp);

            fprintf(out_fp, ",%u,%" PRIu64 ",",
                    nlr->target_attribute, nlr->target_vcn);

            if (fname[0] != '\0') {
                int need_q = (strchr(fname, ',') || strchr(fname, '"') != NULL);
                if (need_q) fprintf(out_fp, "\"%s\"\n", fname);
                else        fprintf(out_fp, "%s\n", fname);
            } else {
                fputc('\n', out_fp);
            }

            count++;
        }

        off += rec_sz;
    }

    return count;
}

/* ============================================================
 *  main
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "使い方: %s <$LogFile バイナリ> [出力CSV]\n"
            "  例:  ./logfile_decode logfile.bin > logfile.csv\n"
            "       ./logfile_decode logfile.bin logfile.csv\n\n"
            "$LogFile 抽出 (The Sleuth Kit):\n"
            "  icat -o <セクタオフセット> image.raw 2 > logfile.bin\n\n"
            "CSV 列:\n"
            "  LSN,PrevLSN,UndoLSN,TransID,RedoOp,UndoOp,"
            "TargetMFT,TargetAttr,VCN,Filename\n",
            argv[0]);
        return 1;
    }

    const char *infile  = argv[1];
    const char *outfile = (argc >= 3) ? argv[2] : NULL;

    /* $LogFile をメモリに読み込む（通常 64 MB 以下） */
    FILE *fp = fopen(infile, "rb");
    if (!fp) { perror(infile); return 1; }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0) {
        fprintf(stderr, "Error: ファイルが空または読み取り失敗\n");
        fclose(fp); return 1;
    }
    if ((unsigned long)fsize > MAX_LOGFILE_SIZE) {
        fprintf(stderr, "Error: ファイルが大きすぎます (%ld MB > %u MB)\n",
                fsize / 1024 / 1024, MAX_LOGFILE_SIZE / 1024 / 1024);
        fclose(fp); return 1;
    }

    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) { perror("malloc"); fclose(fp); return 1; }

    if (fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
        fprintf(stderr, "Error: 読み取り不完全\n");
        free(buf); fclose(fp); return 1;
    }
    fclose(fp);
    fprintf(stderr, "# ファイルサイズ: %ld bytes\n", fsize);

    /* ── Restart Pages（先頭 2 ページ）の解析 ── */
    for (int i = 0; i < 2; i++) {
        uint32_t off = (uint32_t)i * DEFAULT_PAGE_SIZE;
        if (off + DEFAULT_PAGE_SIZE > (uint32_t)fsize) break;
        /* USA 適用前のコピーで magic を確認 */
        const LFS_RESTART_PAGE_HEADER *rp =
            (const LFS_RESTART_PAGE_HEADER *)(buf + off);
        if (rp->magic == MAGIC_RSTR) {
            uint8_t pg[DEFAULT_PAGE_SIZE];
            memcpy(pg, buf + off, DEFAULT_PAGE_SIZE);
            apply_usa(pg, DEFAULT_PAGE_SIZE, rp->usa_offset, rp->usa_count);
            parse_restart_page(pg);
            break;   /* 最初の有効な RSTR だけ使えば十分 */
        }
    }

    /* ── CSV 出力先 ── */
    FILE *out_fp = stdout;
    if (outfile) {
        out_fp = fopen(outfile, "w");
        if (!out_fp) { perror(outfile); free(buf); return 1; }
    }

    /* CSV ヘッダ */
    fprintf(out_fp,
            "LSN,PrevLSN,UndoLSN,TransID,RedoOp,UndoOp,"
            "TargetMFT,TargetAttr,VCN,Filename\n");

    /* ── RCRD ページを順次処理（Restart Pages をスキップ） ── */
    uint64_t page_count = 0;
    uint64_t rec_count  = 0;
    uint32_t off = g_page_size * 2;   /* ページ 0,1 = RSTR → スキップ */

    while (off + g_page_size <= (uint32_t)fsize) {
        const LFS_RECORD_PAGE_HEADER *ph =
            (const LFS_RECORD_PAGE_HEADER *)(buf + off);

        if (ph->magic == MAGIC_RCRD) {
            /* USA 適用のためにページのコピーを作る */
            uint8_t *pg = malloc(g_page_size);
            if (!pg) { perror("malloc"); break; }
            memcpy(pg, buf + off, g_page_size);
            apply_usa(pg, g_page_size, ph->usa_offset, ph->usa_count);
            rec_count += process_rcrd_page(pg, g_page_size, out_fp);
            free(pg);
            page_count++;
        }

        off += g_page_size;
    }

    if (out_fp != stdout) fclose(out_fp);
    free(buf);

    fprintf(stderr, "# ─────────────────────────────────\n");
    fprintf(stderr, "# 処理ページ数 (RCRD) : %" PRIu64 "\n", page_count);
    fprintf(stderr, "# 出力レコード数      : %" PRIu64 "\n", rec_count);
    if (outfile)
        fprintf(stderr, "# 出力ファイル        : %s\n", outfile);

    return 0;
}
