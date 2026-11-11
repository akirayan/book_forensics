/*
 * fatlib.c - FAT ファミリ (FAT32/exFAT) デコーダ共通ライブラリ 実装
 *
 * 旧 fat32_decode.c / exfat_decode.c が各自で重複して持っていた
 * ヘルパ群をここに集約した。CSV 出力・-tz 挙動・タイムスタンプ整形は
 * 旧ツールと完全に同一の結果になるよう移植している。
 */

#define _GNU_SOURCE     /* timegm(3) */

#include "fatlib.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <iconv.h>

/* ============================================================
 *  純粋ヘルパ
 * ============================================================ */

ssize_t fat_full_pread(int fd, void *buf, size_t count, off_t offset) {
    size_t done = 0;
    while (done < count) {
        ssize_t r = pread(fd, (char *)buf + done, count - done,
                          offset + (off_t)done);
        if (r <= 0) return done ? (ssize_t)done : r;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

int fat_parse_tz_offset(const char *s, int *out_min) {
    if (!s || (s[0] != '+' && s[0] != '-')) return -1;
    int sign = (s[0] == '+') ? 1 : -1;
    int hh, mm;
    if (sscanf(s + 1, "%2d%2d", &hh, &mm) != 2) return -1;
    if (hh < 0 || hh > 14 || mm < 0 || mm > 59) return -1;
    *out_min = sign * (hh * 60 + mm);
    return 0;
}

/*
 * ローカル時刻フィールド → UTC の time_t
 *
 * timegm(3) は struct tm を「UTC として」解釈し time_t を返す。
 * ローカル時刻を timegm に渡した後、UTC オフセット (分) を引くことで
 * 正しい UTC の time_t を得る。
 */
static time_t local_to_utc(int year, int month, int day,
                           int hour, int minute, int second,
                           int offset_min) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year  = year - 1900;
    t.tm_mon   = month - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = minute;
    t.tm_sec   = second;
    t.tm_isdst = 0;
    return timegm(&t) - (time_t)(offset_min * 60);
}

void fat_time_to_str(int year, int month, int day,
                     int hour, int minute, int second, int ms,
                     int eff_offset_min, char *buf, size_t buflen) {
    if (eff_offset_min != TZ_NOT_SET) {
        /* UTC に変換して出力 */
        time_t utc_t = local_to_utc(year, month, day,
                                    hour, minute, second,
                                    eff_offset_min);
        struct tm *utc_tm = gmtime(&utc_t);
        if (!utc_tm) { buf[0] = '\0'; return; }
        snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                 utc_tm->tm_year + 1900, utc_tm->tm_mon + 1, utc_tm->tm_mday,
                 utc_tm->tm_hour, utc_tm->tm_min, utc_tm->tm_sec, ms);
    } else {
        /* ローカル時刻のまま出力 */
        snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                 year, month, day, hour, minute, second, ms);
    }
}

int fat_utf16le_to_utf8(const uint8_t *src, size_t src_bytes,
                        char *dst, size_t dst_bytes) {
    if (src_bytes == 0) { dst[0] = '\0'; return 0; }
    iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
    if (cd == (iconv_t)-1) { dst[0] = '\0'; return -1; }
    char   *in   = (char *)src;
    char   *out  = dst;
    size_t  inl  = src_bytes;
    size_t  outl = dst_bytes - 1;
    iconv(cd, &in, &inl, &out, &outl);
    *out = '\0';
    iconv_close(cd);
    return 0;
}

void fat_csv_write_field(FILE *fp, const char *s) {
    fputc('"', fp);
    for (; *s; s++) {
        if (*s == '"') fputc('"', fp);
        fputc(*s, fp);
    }
    fputc('"', fp);
}

/* ============================================================
 *  ボリューム依存ヘルパ
 * ============================================================ */

off_t fat_cluster_to_byte(const fat_vol_t *vol, uint32_t cluster) {
    return (off_t)(vol->heap_offset_b +
                   (uint64_t)(cluster - 2) * vol->bytes_per_clus);
}

DirNode *fat_dir_find(fat_vol_t *vol, uint32_t cluster) {
    DirNode *e = vol->dir_hash[cluster % DIR_HASH_SZ];
    while (e) { if (e->cluster == cluster) return e; e = e->next; }
    return NULL;
}

void fat_dir_add(fat_vol_t *vol, uint32_t cluster,
                 uint32_t parent_cluster, const char *name) {
    DirNode *e = fat_dir_find(vol, cluster);
    if (e) {
        e->parent_cluster = parent_cluster;
        snprintf(e->name, MAX_NAME_UTF8, "%s", name);
        return;
    }
    e = calloc(1, sizeof(DirNode));
    if (!e) { perror("calloc dir_add"); exit(1); }
    e->cluster = cluster; e->parent_cluster = parent_cluster;
    snprintf(e->name, MAX_NAME_UTF8, "%s", name);
    uint32_t h        = cluster % DIR_HASH_SZ;
    e->next           = vol->dir_hash[h];
    vol->dir_hash[h]  = e;
}

void fat_build_fullpath(fat_vol_t *vol, uint32_t parent_cluster,
                        const char *leaf, char *out, size_t out_size) {
    const char *parts[MAX_DEPTH];
    int depth = 0;
    parts[depth++] = leaf;

    uint32_t cur = parent_cluster;
    int guard = 0;
    while (guard++ < 256) {
        if (cur == 0 || cur == vol->root_cluster) break;
        DirNode *e = fat_dir_find(vol, cur);
        if (!e) { if (depth < MAX_DEPTH) parts[depth++] = "<orphan>"; break; }
        if (depth < MAX_DEPTH) parts[depth++] = e->name;
        uint32_t p = e->parent_cluster;
        if (p == cur) { if (depth < MAX_DEPTH) parts[depth++] = "<loop>"; break; }
        cur = p;
    }
    if (guard >= 256 && depth < MAX_DEPTH) parts[depth++] = "<loop>";

    out[0] = '\0';
    size_t pos = 0;
    if (pos + 1 < out_size) { out[pos++] = '/'; out[pos] = '\0'; }
    for (int i = depth - 1; i >= 0; i--) {
        size_t plen = strlen(parts[i]);
        if (pos + plen < out_size) {
            memcpy(out + pos, parts[i], plen);
            pos += plen; out[pos] = '\0';
        }
        if (i > 0 && pos + 1 < out_size) { out[pos++] = '/'; out[pos] = '\0'; }
    }
}

void fat_dir_table_free(fat_vol_t *vol) {
    for (int i = 0; i < DIR_HASH_SZ; i++) {
        DirNode *e = vol->dir_hash[i];
        while (e) { DirNode *nx = e->next; free(e); e = nx; }
        vol->dir_hash[i] = NULL;
    }
}
