/*
 * tm_judge.c - MFT タイムスタンプの Timestomping スコアリング
 *
 * mft_tm_decode が出力した「8 タイムスタンプ CSV」を読み、各レコードを
 * 複数の指標で採点して段階分類する。Unix 流に、取り出す（mft_tm_decode）と
 * 判断する（tm_judge）を分離してある。
 *
 *   sudo ./mft_tm_decode /dev/loop0p1 | ./tm_judge
 *   ./tm_judge timestamps.csv
 *
 * なぜ二値(OK/SUSPECT)をやめたか:
 *   実機の Windows C: では、全ファイルの約 1/4 が「$SI.modified < $FN.modified」で
 *   SUSPECT になる。これは改竄ではなく、コピー・インストール・展開という日常操作が
 *   「内容の更新時刻を保ったまま新しい場所に置く」ために自然に起きる。
 *   単一ルールの二値判定では、コピーと改竄を分離できない。
 *   そこで指標を重み付けして合算し、段階（tier）で絞り込む。
 *
 * 指標（score 加点。実機 C: の分布で較正）:
 *   m +1  $SI.modified < $FN.modified          … コピーでも立つ（弱い）
 *   c +1  $SI.created  < $FN.created            … 同上
 *   z +1  $SI.modified のサブ秒が 0（$FN は非 0）… 秒丸め。パッケージ由来でも出る（弱い）
 *   e +1  $SI の created=modified=accessed が一致 … 未更新ファイルでも出る（弱い）
 *   M +3  $SI.modified がちょうど真夜中(00:00:00.0000000) … 丸い既定値（強い）
 *   B +4  $SI.modified < baseline(-b)           … OS 導入より前＝説明が難しい（強い）
 *
 *   弱い指標（m/c/z/e）はコピー・インストール・パッケージ由来で日常的に立つので、
 *   それだけでは STOMP に届かない。真夜中(M)やベースライン(B)という強い指標が
 *   絡んで初めて STOMP になる ―― ここが二値判定との違い。
 *
 * tier（score で段階化。-b 無しの既定しきい値）:
 *   OK     score 0          矛盾なし
 *   COPY   score 1-2        コピー/インストールで説明がつく（偽陽性になりやすい）
 *   REVIEW score 3-5        一応見ておく（弱い指標が重なっただけ）
 *   STOMP  score >= 6       強い指標が絡む。ツール改竄の疑いが濃い（まずここだけ精査）
 *
 * コンパイル:
 *   cc -O2 -Wall -o tm_judge tm_judge.c   # Linux / macOS 共通（iconv 不要）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLINE   8192
#define NFIELD    16
#define SCORE_CAP 15   /* ヒストグラムの上限 */

/* tier しきい値（score -> tier 境界）。-b 指定時も同じ段階を使う。 */
#define TH_COPY    1   /* >=1 : COPY */
#define TH_REVIEW  3   /* >=3 : REVIEW */
#define TH_STOMP   6   /* >=6 : STOMP  */

/* 1 行をカンマで分割し、各フィールドの前後のダブルクォートを剥がす。
 * タイムスタンプはカンマを含まないので、先頭 10 フィールドは確実に取れる。
 * 11 番目以降（ファイル名にカンマが入った場合）は使わないので無視。 */
static int split_csv(char *line, char *out[], int maxf) {
    int n = 0;
    char *p = line;
    while (n < maxf) {
        out[n] = p;
        char *comma = strchr(p, ',');
        if (comma) { *comma = '\0'; p = comma + 1; n++; }
        else { n++; break; }
    }
    /* クォート除去 */
    for (int i = 0; i < n; i++) {
        char *s = out[i];
        size_t len = strlen(s);
        if (len >= 2 && s[0] == '"' && s[len-1] == '"') {
            s[len-1] = '\0';
            out[i] = s + 1;
        }
    }
    return n;
}

/* 末尾が ".0000000"（サブ秒ゼロ）か */
static int subsec_zero(const char *ts) {
    size_t len = strlen(ts);
    return (len >= 8 && strcmp(ts + len - 8, ".0000000") == 0);
}

/* ちょうど真夜中（" 00:00:00.0000000" で終わる）か */
static int is_midnight(const char *ts) {
    size_t len = strlen(ts);
    const char *suf = " 00:00:00.0000000";
    size_t slen = strlen(suf);
    return (len >= slen && strcmp(ts + len - slen, suf) == 0);
}

static const char *tier_of(int score) {
    if (score >= TH_STOMP)  return "STOMP";
    if (score >= TH_REVIEW) return "REVIEW";
    if (score >= TH_COPY)   return "COPY";
    return "OK";
}

int main(int argc, char *argv[]) {
    const char *baseline = NULL;  /* -b "YYYY-MM-DD HH:MM:SS..." */
    int   min_score = -1;         /* -m N : score>=N の行だけ出力 */
    const char *only_tier = NULL; /* -t TIER : その tier だけ出力 */
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc)      baseline  = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) min_score = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) only_tier = argv[++i];
        else if (argv[i][0] != '-')                      path = argv[i];
        else {
            fprintf(stderr,
                "使い方: %s [-b \"<baseline datetime>\"] [-m <min_score>] [-t <tier>] [in.csv]\n"
                "  mft_tm_decode の CSV を標準入力かファイルから読む。\n", argv[0]);
            return 1;
        }
    }

    FILE *in = stdin;
    if (path) {
        in = fopen(path, "r");
        if (!in) { fprintf(stderr, "Error: '%s' を開けません\n", path); return 1; }
    }

    printf("mft_ref,filename,score,tier,flags\n");

    char line[MAXLINE];
    char *f[NFIELD];
    unsigned long long hist[SCORE_CAP + 1] = {0};
    unsigned long long n_ok = 0, n_copy = 0, n_review = 0, n_stomp = 0, n_rows = 0;

    while (fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;       /* コメント */
        if (!strncmp(line, "mft_ref", 7)) continue;            /* ヘッダ */

        int nf = split_csv(line, f, NFIELD);
        if (nf < 11) continue;

        const char *ref   = f[0];
        const char *si_cr = f[2], *si_mo = f[3], *si_ac = f[5];
        const char *fn_cr = f[6], *fn_mo = f[7];
        const char *name  = f[10];

        int score = 0;
        char flags[8]; int fi = 0;

        if (*si_mo && *fn_mo && strcmp(si_mo, fn_mo) < 0) { score += 1; flags[fi++]='m'; }
        if (*si_cr && *fn_cr && strcmp(si_cr, fn_cr) < 0) { score += 1; flags[fi++]='c'; }
        if (subsec_zero(si_mo) && *fn_mo && !subsec_zero(fn_mo)) { score += 1; flags[fi++]='z'; }
        if (*si_cr && *si_mo && *si_ac &&
            !strcmp(si_cr, si_mo) && !strcmp(si_mo, si_ac))      { score += 1; flags[fi++]='e'; }
        if (is_midnight(si_mo))                                 { score += 3; flags[fi++]='M'; }
        if (baseline && *si_mo && strcmp(si_mo, baseline) < 0)  { score += 3; flags[fi++]='B'; }
        flags[fi] = '\0';
        if (fi == 0) { flags[0]='-'; flags[1]='\0'; }

        const char *tier = tier_of(score);
        n_rows++;
        if      (!strcmp(tier, "STOMP"))  n_stomp++;
        else if (!strcmp(tier, "REVIEW")) n_review++;
        else if (!strcmp(tier, "COPY"))   n_copy++;
        else                               n_ok++;
        hist[score > SCORE_CAP ? SCORE_CAP : score]++;

        int show = 1;
        if (min_score >= 0 && score < min_score) show = 0;
        if (only_tier && strcmp(tier, only_tier)) show = 0;
        if (show)
            printf("%s,%s,%d,%s,%s\n", ref, name, score, tier, flags);
    }
    if (in != stdin) fclose(in);

    fprintf(stderr, "# ── tm_judge 集計 ──\n");
    fprintf(stderr, "# 評価レコード : %llu\n", n_rows);
    fprintf(stderr, "# OK           : %llu\n", n_ok);
    fprintf(stderr, "# COPY         : %llu\n", n_copy);
    fprintf(stderr, "# REVIEW       : %llu\n", n_review);
    fprintf(stderr, "# STOMP        : %llu\n", n_stomp);
    fprintf(stderr, "# score 分布    :");
    for (int s = 0; s <= SCORE_CAP; s++)
        if (hist[s]) fprintf(stderr, " %d=%llu", s, hist[s]);
    fprintf(stderr, "\n");
    if (baseline) fprintf(stderr, "# baseline     : %s\n", baseline);
    return 0;
}
