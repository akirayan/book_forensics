/*
 * reg_poladtev.c
 *
 *  version 0.1  2026-11-24  initial release. Structure derived byte-by-byte
 *                           from two real SECURITY hives (Win10 Home 1804 /
 *                           Win11 2026) and cross-checked against kazamiya's
 *                           "PolAdtEv_Structure" rev5 (2024-09-16),
 *                           https://www.kazamiya.net/poladtev .
 *  version 0.2  2026-11-24  Added English subcategory names (exact auditpol
 *                           strings) + category GUIDs, verified against
 *                           `auditpol /get /category:*` and
 *                           `auditpol /list /category /v` on Win11 ja/en.
 *                           Added the 12th Logon/Logoff subcategory
 *                           "Access Rights" (24H2+). --en flag.
 *  version 0.3  2026-11-24  Print usage and exit when stdin is a terminal or
 *                           empty, instead of blocking on an interactive read.
 *  version 0.4  2026-11-25  --en now also localizes CSV. CSV has a single
 *                           Subcategory column (and Category/Setting) that
 *                           follow --en; default Japanese, --en English
 *                           (matching auditpol). RawHex keeps the raw 0..3.
 *  version 0.5  2026-11-26  --csv now reproduces the `auditpol /backup` column
 *                           layout: Machine Name,Policy Target,Subcategory,
 *                           Subcategory GUID,Inclusion Setting,Exclusion
 *                           Setting,Setting Value. Machine Name is left blank
 *                           (a dead hive carries no host name); Policy Target
 *                           is System; Inclusion Setting uses auditpol's
 *                           English tokens; Setting Value is the raw 0..3.
 *                           Added the per-subcategory GUIDs ({0CCE92xx-...}).
 *  version 0.6  2026-11-26  Default output is now English; --ja switches to
 *                           Japanese. (--en still accepted, now the default.)
 *  (c) Akira Endo (yan.akira@gmail.com)
 *
 *
 * Purpose:
 *   `auditpol.exe /get /category:*` only works on a running system. On a dead
 *   disk the effective *local* audit policy must be read straight from:
 *
 *       HKLM\SECURITY\Policy\PolAdtEv   (REG_NONE)
 *
 *   This decoder parses that blob and prints, per subcategory, whether
 *   Success / Failure auditing was enabled -- i.e. which events the machine
 *   was configured to record at all. It is the premise of Security.evtx
 *   analysis: "no event" means EITHER "nothing happened" OR "auditing was
 *   off", and only PolAdtEv tells the two apart.
 *
 * Usage (prefer -f -p: it emits just this value, two lines):
 *   regf_decode -f -p "/ROOT/Policy/PolAdtEv" SECURITY | reg_poladtev
 *   reg_poladtev --ja  < poladtev.hex     (Japanese subcategory names)
 *   reg_poladtev --csv < poladtev.hex
 *   # regf_decode -d "*PolAdtEv/(default)" also feeds this tool, but -d only
 *   # selects which value to DUMP in full hex -- it is NOT a filter, so it
 *   # still prints the entire hive tree. -f -p is cleaner for a single value.
 *
 *   Options:
 *     (default)     English output (names + settings match `auditpol /get`).
 *     -j | --ja     Japanese output. Applies to the human view and to the CSV
 *                   Subcategory column (the CSV Inclusion Setting is always
 *                   English, as auditpol /backup writes it).
 *     -e | --en     English output (the default; accepted for explicitness).
 *     -c | --csv    CSV in `auditpol /backup` layout:
 *                     Machine Name,Policy Target,Subcategory,Subcategory GUID,
 *                     Inclusion Setting,Exclusion Setting,Setting Value
 *                   Machine Name is blank (a dead hive has no host name);
 *                   Setting Value is the raw 0..3.
 *     -h | --help
 *
 * Expected Input (auto-detected):
 *   (1) regf_decode pipe:  .../Policy/PolAdtEv/(default)|REG_NONE|00 01 00 00 ...
 *       NOTE: the plain tree output truncates long values ("...[152 bytes]").
 *       Use -f -p to emit just this value in full. -d also emits full hex, but
 *       -d selects the dump target (not a filter) so it prints the whole tree.
 *   (2) regedit.exe hex:   "(default)"=hex:00,01,00,00,09,00,...,\
 *                            04,00
 *   (3) raw hex stream:    00 01 00 00 ...
 *   The header gives the exact length, so trailing bytes (other values in the
 *   same stream) are ignored.
 *
 * Structure (empirically derived; kazamiya rev5 confirms):
 *   Header (12 bytes, six 16-bit LE words):
 *     off 0x00 word 0x0100  (kazamiya: unknown; conventionally "version")
 *     off 0x02 word 0x0000  unknown
 *     off 0x04 word nCat    number of categories (observed 9)
 *     off 0x06 word 0x0000  unknown
 *     off 0x08 word cntOff  byte offset to the per-category count array
 *     off 0x0A word 0x0000  unknown
 *   Settings region [0x0C .. cntOff-2):
 *     one 16-bit LE word per subcategory, in canonical BYTE order, grouped by
 *     the count array. 0=None 1=Success 2=Failure 3=Success and Failure.
 *   Footer:
 *     off cntOff-2 word     unknown (varies by build; kazamiya lists
 *                           1607:0C DB 2016:33 CC 2019:04 09
 *                           11 23H2:D1 BA 2022:78 19 -> opaque)
 *     off cntOff   word[nCat]  subcategory count of each category
 *   total length = cntOff + nCat*2
 *
 * Forensic Notes:
 *   - PolAdtEv's internal BYTE order is NOT auditpol's display order. e.g. the
 *     System category stores "Security State Change" first, but `auditpol
 *     /get` prints it last. This tool shows BYTE order (what is actually on
 *     disk); the names were matched to auditpol by GUID/meaning, not position.
 *   - Microsoft *appends* subcategories in new builds: Logon/Logoff went 11
 *     (<=23H2) -> 12 with "Access Rights" (24H2+). This tool groups by the
 *     on-disk count array (always correct) and overlays Win11 canonical names;
 *     a slot past the known list is flagged UNKNOWN rather than guessed.
 *   - The two header "unknown" words and the footer "unknown" word are left
 *     labelled unknown: the reference parser cannot name them either. Do not
 *     invent meaning for bytes you cannot ground.
 *   - GPO-pushed advanced policy also lands in audit.csv; later `auditpol /set`
 *     edits audit.csv and may diverge from PolAdtEv. Compare, don't trust one.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* isatty */

/* ------------------------------------------------------------------ */
/* Canonical tables (Windows 11). Subcategories are in PolAdtEv BYTE   */
/* order. English strings are the exact `auditpol /get` display names; */
/* category GUIDs are from `auditpol /list /category /v`.              */
/* ------------------------------------------------------------------ */

typedef struct { const char *jp; const char *en; } SubName;

static const char *CAT_JP[9] = {
    "システム", "ログオン/ログオフ", "オブジェクト アクセス", "特権の使用",
    "詳細追跡", "ポリシーの変更", "アカウント管理", "DS アクセス",
    "アカウント ログオン",
};
static const char *CAT_EN[9] = {
    "System", "Logon/Logoff", "Object Access", "Privilege Use",
    "Detailed Tracking", "Policy Change", "Account Management", "DS Access",
    "Account Logon",
};
static const char *CAT_GUID[9] = {
    "{69979848-797A-11D9-BED3-505054503030}",  /* System */
    "{69979849-797A-11D9-BED3-505054503030}",  /* Logon/Logoff */
    "{6997984A-797A-11D9-BED3-505054503030}",  /* Object Access */
    "{6997984B-797A-11D9-BED3-505054503030}",  /* Privilege Use */
    "{6997984C-797A-11D9-BED3-505054503030}",  /* Detailed Tracking */
    "{6997984D-797A-11D9-BED3-505054503030}",  /* Policy Change */
    "{6997984E-797A-11D9-BED3-505054503030}",  /* Account Management */
    "{6997984F-797A-11D9-BED3-505054503030}",  /* DS Access */
    "{69979850-797A-11D9-BED3-505054503030}",  /* Account Logon */
};

static const SubName SUB_SYSTEM[] = {
    {"セキュリティ状態の変更",       "Security State Change"},
    {"セキュリティ システムの拡張",  "Security System Extension"},
    {"システムの整合性",             "System Integrity"},
    {"IPsec ドライバー",             "IPsec Driver"},
    {"その他のシステム イベント",    "Other System Events"},
};
static const SubName SUB_LOGON[] = {
    {"ログオン",                        "Logon"},
    {"ログオフ",                        "Logoff"},
    {"アカウント ロックアウト",         "Account Lockout"},
    {"IPsec メイン モード",             "IPsec Main Mode"},
    {"特殊なログオン",                  "Special Logon"},
    {"IPsec クイック モード",           "IPsec Quick Mode"},
    {"IPsec 拡張モード",                "IPsec Extended Mode"},
    {"その他のログオン/ログオフ イベント", "Other Logon/Logoff Events"},
    {"ネットワーク ポリシー サーバー",  "Network Policy Server"},
    {"ユーザー要求/デバイスの信頼性情報", "User / Device Claims"},
    {"監査グループのメンバーシップ",    "Group Membership"},
    {"アクセス権",                      "Access Rights"},  /* 24H2+ (append) */
};
static const SubName SUB_OBJECT[] = {
    {"ファイル システム",             "File System"},
    {"レジストリ",                     "Registry"},
    {"カーネル オブジェクト",          "Kernel Object"},
    {"SAM",                            "SAM"},
    {"その他のオブジェクト アクセス イベント", "Other Object Access Events"},
    {"証明書サービス",                 "Certification Services"},
    {"生成されたアプリケーション",     "Application Generated"},
    {"ハンドル操作",                   "Handle Manipulation"},
    {"ファイルの共有",                 "File Share"},
    {"フィルタリング プラットフォーム パケットのドロップ", "Filtering Platform Packet Drop"},
    {"フィルタリング プラットフォームの接続", "Filtering Platform Connection"},
    {"詳細なファイル共有",             "Detailed File Share"},
    {"リムーバブル記憶域",             "Removable Storage"},
    {"集約型ポリシー ステージング",    "Central Policy Staging"},
};
static const SubName SUB_PRIV[] = {
    {"重要な特権の使用",       "Sensitive Privilege Use"},
    {"重要でない特権の使用",   "Non Sensitive Privilege Use"},
    {"その他の特権の使用イベント", "Other Privilege Use Events"},
};
static const SubName SUB_DETAIL[] = {
    {"プロセス作成",              "Process Creation"},
    {"プロセス終了",              "Process Termination"},
    {"DPAPI アクティビティ",      "DPAPI Activity"},
    {"RPC イベント",              "RPC Events"},
    {"プラグ アンド プレイ イベント", "Plug and Play Events"},
    {"Token Right Adjusted Events", "Token Right Adjusted Events"},
};
static const SubName SUB_POLICY[] = {
    {"監査ポリシーの変更",         "Audit Policy Change"},
    {"認証ポリシーの変更",         "Authentication Policy Change"},
    {"承認ポリシーの変更",         "Authorization Policy Change"},
    {"MPSSVC ルールレベル ポリシーの変更", "MPSSVC Rule-Level Policy Change"},
    {"フィルタリング プラットフォーム ポリシーの変更", "Filtering Platform Policy Change"},
    {"その他のポリシー変更イベント", "Other Policy Change Events"},
};
static const SubName SUB_ACCTMGMT[] = {
    {"ユーザー アカウント管理",       "User Account Management"},
    {"コンピューター アカウント管理", "Computer Account Management"},
    {"セキュリティ グループ管理",     "Security Group Management"},
    {"配布グループの管理",            "Distribution Group Management"},
    {"アプリケーション グループの管理", "Application Group Management"},
    {"その他のアカウント管理イベント", "Other Account Management Events"},
};
static const SubName SUB_DSACCESS[] = {
    {"ディレクトリ サービス アクセス",       "Directory Service Access"},
    {"ディレクトリ サービスの変更",          "Directory Service Changes"},
    {"ディレクトリ サービスのレプリケーション", "Directory Service Replication"},
    {"詳細なディレクトリ サービス レプリケーション", "Detailed Directory Service Replication"},
};
static const SubName SUB_ACCTLOGON[] = {
    {"資格情報の確認",                 "Credential Validation"},
    {"Kerberos サービス チケット操作", "Kerberos Service Ticket Operations"},
    {"その他のアカウント ログオン イベント", "Other Account Logon Events"},
    {"Kerberos 認証サービス",          "Kerberos Authentication Service"},
};

static const SubName *SUBS[9] = {
    SUB_SYSTEM, SUB_LOGON, SUB_OBJECT, SUB_PRIV, SUB_DETAIL,
    SUB_POLICY, SUB_ACCTMGMT, SUB_DSACCESS, SUB_ACCTLOGON,
};
static const int SUBS_N[9] = { 5, 12, 14, 3, 6, 6, 6, 4, 4 };  /* Win11 24H2 */

/* Per-subcategory GUIDs ({0CCE92xx-69AE-11D9-BED3-505054503030}), in the same */
/* PolAdtEv byte order as the SUB_* tables above. Matches auditpol's           */
/* "Subcategory GUID" column (auditpol /list /subcategory /v).                  */
#define AP(h) "{0CCE92" h "-69AE-11D9-BED3-505054503030}"
static const char *GUID_SYSTEM[]   = { AP("10"), AP("11"), AP("12"), AP("13"), AP("14") };
static const char *GUID_LOGON[]    = { AP("15"), AP("16"), AP("17"), AP("18"), AP("1B"),
                                       AP("19"), AP("1A"), AP("1C"), AP("43"), AP("47"),
                                       AP("49"), AP("4B") };
static const char *GUID_OBJECT[]   = { AP("1D"), AP("1E"), AP("1F"), AP("20"), AP("27"),
                                       AP("21"), AP("22"), AP("23"), AP("24"), AP("25"),
                                       AP("26"), AP("44"), AP("45"), AP("46") };
static const char *GUID_PRIV[]     = { AP("28"), AP("29"), AP("2A") };
static const char *GUID_DETAIL[]   = { AP("2B"), AP("2C"), AP("2D"), AP("2E"), AP("48"), AP("4A") };
static const char *GUID_POLICY[]   = { AP("2F"), AP("30"), AP("31"), AP("32"), AP("33"), AP("34") };
static const char *GUID_ACCTMGMT[] = { AP("35"), AP("36"), AP("37"), AP("38"), AP("39"), AP("3A") };
static const char *GUID_DSACCESS[] = { AP("3B"), AP("3C"), AP("3D"), AP("3E") };
static const char *GUID_ACCTLOGON[]= { AP("3F"), AP("40"), AP("41"), AP("42") };
#undef AP
static const char *const *SUB_GUID[9] = {
    GUID_SYSTEM, GUID_LOGON, GUID_OBJECT, GUID_PRIV, GUID_DETAIL,
    GUID_POLICY, GUID_ACCTMGMT, GUID_DSACCESS, GUID_ACCTLOGON,
};

/* ------------------------------------------------------------------ */

static int g_csv = 0;
static int g_en  = 1;   /* default English; --ja switches to Japanese */

static const char *setting_jp(unsigned v)
{
    switch (v) { case 0: return "監査なし"; case 1: return "成功";
                 case 2: return "失敗";    case 3: return "成功および失敗";
                 default: return "(不明な値)"; }
}
static const char *setting_en(unsigned v)
{
    switch (v) { case 0: return "No Auditing"; case 1: return "Success";
                 case 2: return "Failure";     case 3: return "Success and Failure";
                 default: return "Unknown"; }
}
/* ------------------------------------------------------------------ */

static char *slurp_stdin(size_t *out_len)
{
    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
    }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t collect_hex(const char *p, uint8_t *out, size_t outcap,
                          size_t want, int *truncated)
{
    size_t n = 0;
    int hi = -1;
    *truncated = 0;
    while (*p && n < outcap) {
        if (p[0] == '.' && p[1] == '.' && p[2] == '.') { *truncated = 1; break; }
        int v = hexval((unsigned char)*p);
        if (v < 0) { p++; continue; }
        if (hi < 0) { hi = v; }
        else { out[n++] = (uint8_t)((hi << 4) | v); hi = -1;
               if (want && n >= want) break; }
        p++;
    }
    return n;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--ja] [--csv]   (default output is English; --ja = Japanese)\n"
        "  Decode HKLM\\SECURITY\\Policy\\PolAdtEv (audit policy blob).\n"
        "    regf_decode -f -p \"/ROOT/Policy/PolAdtEv\" SECURITY | %s\n"
        "  (-d \"*PolAdtEv/(default)\" also works but prints the whole tree.)\n",
        prog, prog);
}

int main(int argc, char **argv)
{
    const char *prog = "reg_poladtev";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--csv")) g_csv = 1;
        else if (!strcmp(argv[i], "-j") || !strcmp(argv[i], "--ja")) g_en = 0;
        else if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--en")) g_en = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(prog); return 0; }
        else { fprintf(stderr, "%s: unknown option: %s\n", prog, argv[i]); usage(prog); return 2; }
    }

    /* No pipe/redirect: stdin is the terminal. Don't block waiting for a
     * value that will never come -- show how to use the tool and exit. */
    if (isatty(fileno(stdin))) {
        fprintf(stderr, "%s: no input on stdin.\n", prog);
        usage(prog);
        return 2;
    }

    size_t raw_len = 0;
    char *raw = slurp_stdin(&raw_len);
    if (!raw) { fprintf(stderr, "%s: out of memory\n", prog); return 1; }
    if (raw_len == 0) {
        fprintf(stderr, "%s: empty input.\n", prog);
        usage(prog);
        free(raw);
        return 2;
    }

    const char *start;
    const char *h = strstr(raw, "hex:");
    if (h) start = h + 4;
    else {
        const char *rn = strstr(raw, "REG_NONE");  /* covers |REG_NONE| and REG_NONE<ws> */
        start = rn ? rn + 8 : raw;
    }

    uint8_t buf[4096];
    int truncated = 0;
    size_t got = collect_hex(start, buf, sizeof(buf), 0, &truncated);

    if (got < 12) {
        if (truncated)
            fprintf(stderr, "%s: input looks truncated (\"...[NN bytes]\"). "
                "Re-run regf_decode with -f or -d.\n", prog);
        else
            fprintf(stderr, "%s: not enough data (%zu bytes).\n", prog, got);
        free(raw); return 1;
    }

    unsigned w_ver  = buf[0]  | (buf[1]  << 8);
    unsigned w_unk1 = buf[2]  | (buf[3]  << 8);
    unsigned nCat   = buf[4]  | (buf[5]  << 8);
    unsigned w_unk2 = buf[6]  | (buf[7]  << 8);
    unsigned cntOff = buf[8]  | (buf[9]  << 8);
    unsigned w_unk3 = buf[10] | (buf[11] << 8);

    if (nCat == 0 || nCat > 64 || cntOff < 12 || (cntOff & 1)) {
        fprintf(stderr, "%s: implausible header (nCat=%u, cntOff=0x%x). "
            "Not a PolAdtEv value?\n", prog, nCat, cntOff);
        free(raw); return 1;
    }

    size_t total = (size_t)cntOff + (size_t)nCat * 2;
    if (total > sizeof(buf)) { fprintf(stderr, "%s: value too large.\n", prog); free(raw); return 1; }
    if (got < total) {
        fprintf(stderr, "%s: %s: need %zu bytes, got %zu.%s\n", prog,
            truncated ? "input truncated" : "short value", total, got,
            truncated ? " Use regf_decode -f or -d." : "");
        free(raw); return 1;
    }

    unsigned foot_unk = buf[cntOff - 2] | (buf[cntOff - 1] << 8);
    unsigned counts[64];
    size_t sum = 0;
    for (unsigned i = 0; i < nCat; i++) {
        size_t off = (size_t)cntOff + i * 2;
        counts[i] = buf[off] | (buf[off + 1] << 8);
        sum += counts[i];
    }
    size_t settings_words = ((cntOff - 2) - 12) / 2;
    if (sum != settings_words)
        fprintf(stderr, "%s: WARNING: counts sum=%zu but settings region=%zu "
            "words; decoding by on-disk counts.\n", prog, sum, settings_words);

    if (g_csv) {
        printf("Machine Name,Policy Target,Subcategory,Subcategory GUID,"
               "Inclusion Setting,Exclusion Setting,Setting Value\n");
    } else {
        printf("PolAdtEv  (HKLM\\SECURITY\\Policy\\PolAdtEv)\n");
        printf("  header : ver=0x%04X  categories=%u  count-offset=0x%X\n",
               w_ver, nCat, cntOff);
        printf("  unknown: hdr[0x02]=0x%04X hdr[0x06]=0x%04X hdr[0x0A]=0x%04X"
               "  footer[0x%X]=0x%04X  %s\n",
               w_unk1, w_unk2, w_unk3, cntOff - 2, foot_unk,
               g_en ? "(still unconfirmed, even in kazamiya rev5)"
                    : "(kazamiya rev5 でも未確定)");
        printf("  note   : %s\n",
               g_en ? "order is PolAdtEv byte order, not the `auditpol /get` display order"
                    : "表示はPolAdtEvのバイト順。auditpol /get の表示順とは異なる");
    }

    size_t widx = 0;
    size_t sbase = 12;
    for (unsigned ci = 0; ci < nCat; ci++) {
        const char *cjp = (ci < 9) ? CAT_JP[ci] : "(未知カテゴリ)";
        const char *cen = (ci < 9) ? CAT_EN[ci] : "(unknown)";
        const char *cg  = (ci < 9) ? CAT_GUID[ci] : "";
        if (!g_csv)
            printf("\n■ %s  (%s)  %s   %u\n", cjp, cen, cg, counts[ci]);
        for (unsigned j = 0; j < counts[ci]; j++) {
            size_t boff = sbase + (widx + j) * 2;
            if (boff + 1 >= cntOff - 2) break;
            unsigned val = buf[boff] | (buf[boff + 1] << 8);
            unsigned low = val & 0xFF;
            int known = (ci < 9 && (int)j < SUBS_N[ci]);
            const char *njp = known ? SUBS[ci][j].jp : "(不明: このビルドで追加)";
            const char *nen = known ? SUBS[ci][j].en : "UNKNOWN";
            if (g_csv) {
                /* auditpol /backup layout: Machine Name blank (dead hive has
                   no host name), Policy Target=System, Inclusion Setting in
                   auditpol's English tokens, Exclusion Setting blank,
                   Setting Value = raw 0..3. Subcategory follows --en. */
                const char *sg = known ? SUB_GUID[ci][j] : "";
                printf(",System,%s,%s,%s,,%u\n",
                       g_en ? nen : njp, sg, setting_en(low), low);
            }
            else if (g_en)
                printf("    [%2u] %-42s %s\n", j, nen, setting_en(low));
            else
                printf("    [%2u] %-40s %s\n", j, njp, setting_jp(low));
        }
        widx += counts[ci];
    }

    free(raw);
    return 0;
}
