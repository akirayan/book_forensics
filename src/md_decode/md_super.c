/*
 * md_super.c  ── Linux MD (mdadm) software-RAID superblock decoder (v1.x)
 *
 * 目的: 再構成そのものは mdadm に任せる。本ツールがやるのは、各メンバーの
 *       スーパーブロックを「読み取り専用」で読み解き、RAID レベル・chunk
 *       サイズ・ディスク台数・このディスクの役割(並び順)をバイトから確認すること。
 *
 * 対応: v1.x (magic 0xA92B4EFC)。既定オフセットは v1.2 の 4096 バイト。
 *       v1.1 は 0、v1.0 は末尾付近(末尾 -8KiB を 4KiB 境界に切り下げ)。
 *       旧 v0.90 は末尾 64KiB 付近・別レイアウトで、本ツールは非対応。
 *
 * 注意: リトルエンディアン環境(x86 / arm64)を前提に構造体を直読みする。
 *
 * コンパイル:
 *   cc  -O2 -Wall -o md_super md_super.c   # macOS
 *   gcc -O2 -Wall -o md_super md_super.c   # Linux
 *
 * 実行例:
 *   ./md_super /dev/sdb1          # v1.2 (先頭 4KiB) を読む
 *   ./md_super disk1.img 4096     # イメージのオフセット指定
 *   ./md_super disk1.img end      # 末尾から v1.0 を探す
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define MD_SB_MAGIC 0xA92B4EFCU

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;            /* 0x00: 0xA92B4EFC */
    uint32_t major_version;    /* 0x04: 1 */
    uint32_t feature_map;      /* 0x08 */
    uint32_t pad0;             /* 0x0C */
    uint8_t  set_uuid[16];     /* 0x10: アレイ全体の UUID */
    char     set_name[32];     /* 0x20: アレイ名 (例 "host:0") */
    uint64_t ctime;            /* 0x40: 作成時刻 (下位 40bit = 秒) */
    int32_t  level;            /* 0x48: RAID レベル */
    uint32_t layout;           /* 0x4C: レイアウト (RAID5/10 で意味を持つ) */
    uint64_t size;             /* 0x50: 構成デバイスの使用サイズ (512B 単位) */
    uint32_t chunksize;        /* 0x58: chunk サイズ (512B 単位) */
    uint32_t raid_disks;       /* 0x5C: アレイのディスク台数 */
    uint32_t bitmap_offset;    /* 0x60 */
    uint32_t new_level;        /* 0x64 */
    uint64_t reshape_position; /* 0x68 */
    uint32_t delta_disks;      /* 0x70 */
    uint32_t new_layout;       /* 0x74 */
    uint32_t new_chunk;        /* 0x78 */
    uint32_t new_offset;       /* 0x7C */
    uint64_t data_offset;      /* 0x80: データ開始 (このデバイス内, 512B 単位) */
    uint64_t data_size;        /* 0x88 */
    uint64_t super_offset;     /* 0x90: このスーパーブロックの位置 (512B 単位) */
    uint64_t recovery_offset;  /* 0x98 */
    uint32_t dev_number;       /* 0xA0: このデバイスの識別番号 */
    uint32_t cnt_corrected_read;/* 0xA4 */
    uint8_t  device_uuid[16];  /* 0xA8: このデバイス固有の UUID */
    uint8_t  devflags;         /* 0xB8 */
    uint8_t  bblog_shift;      /* 0xB9 */
    uint16_t bblog_size;       /* 0xBA */
    uint32_t bblog_offset;     /* 0xBC */
    uint64_t utime;            /* 0xC0: 最終更新時刻 */
    uint64_t events;           /* 0xC8: 更新回数(世代)。どれが最新かの判断に使う */
    uint64_t resync_offset;    /* 0xD0 */
    uint32_t sb_csum;          /* 0xD8 */
    uint32_t max_dev;          /* 0xDC: dev_roles[] の有効数 */
    uint8_t  pad3[32];         /* 0xE0: ここまでで 256 バイト (0x100) */
} MDP_SB_1;                     /* dev_roles[] は 0x100 以降に続く */
#pragma pack(pop)

static const char *level_name(int32_t lv)
{
    switch (lv) {
    case 0:  return "RAID0 (ストライプ)";
    case 1:  return "RAID1 (ミラー)";
    case 4:  return "RAID4";
    case 5:  return "RAID5 (分散パリティ)";
    case 6:  return "RAID6 (二重パリティ)";
    case 10: return "RAID10";
    case -1: return "linear";
    case -4: return "multipath";
    default: return "unknown";
    }
}

static const char *role_name(uint16_t r, char *buf, size_t n)
{
    if (r == 0xFFFF) return "spare (予備)";
    if (r == 0xFFFE) return "faulty (故障)";
    snprintf(buf, n, "data slot %u", r);
    return buf;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <device|image> [offset_bytes | end]\n"
            "  offset 既定: 4096 (v1.2)。 0 は v1.1、'end' は末尾から v1.0 を探す\n",
            argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    off_t dev_size = lseek(fd, 0, SEEK_END);   /* 末尾検索(v1.0)用 */

    off_t off = 4096;                          /* 既定: v1.2 */
    if (argc >= 3) {
        if (strcmp(argv[2], "end") == 0)
            off = (dev_size - 8 * 1024) & ~(off_t)(4 * 1024 - 1);
        else
            off = (off_t)strtoull(argv[2], NULL, 0);
    }

    /* union でアラインメントを保証しつつ、可変長の dev_roles[] も読み込む */
    union { uint8_t raw[4096]; MDP_SB_1 sb; } u;
    if (pread(fd, u.raw, sizeof(u.raw), off) != (ssize_t)sizeof(u.raw)) {
        perror("pread"); close(fd); return 1;
    }
    close(fd);

    MDP_SB_1 *sb = &u.sb;
    if (sb->magic != MD_SB_MAGIC) {
        fprintf(stderr,
            "Error: MD スーパーブロックが無い (offset 0x%llX, magic=0x%08X)\n"
            "       v1.2 は 4096、v1.1 は 0、v1.0 は 'end' を試すこと。\n",
            (unsigned long long)off, sb->magic);
        return 1;
    }

    /* dev_roles[] は構造体の直後 (0x100) に並ぶ。role = アレイ内の並び順 */
    const uint16_t *roles = (const uint16_t *)(u.raw + 256);

    char rbuf[64];
    uint32_t chunk_b = sb->chunksize * 512u;
    uint16_t my_role = (sb->dev_number < sb->max_dev)
                     ? roles[sb->dev_number] : 0xFFFF;

    printf("Source                 : %s (offset 0x%llX)\n",
           argv[1], (unsigned long long)off);
    printf("Magic / version        : 0x%08X / v1.x\n", sb->magic);
    printf("Array name (set_name)  : %.32s\n", sb->set_name);
    printf("RAID level             : %s\n", level_name(sb->level));
    printf("RAID disks             : %u\n", sb->raid_disks);
    printf("Chunk size             : %u B (%u KiB)\n", chunk_b, chunk_b / 1024);
    printf("Array member size      : %llu B\n",
           (unsigned long long)sb->size * 512ull);
    printf("Data offset            : %llu sectors (%llu B)\n",
           (unsigned long long)sb->data_offset,
           (unsigned long long)sb->data_offset * 512ull);
    printf("This device number     : %u\n", sb->dev_number);
    printf("This device role       : %s\n", role_name(my_role, rbuf, sizeof rbuf));
    printf("Events (generation)    : %llu\n", (unsigned long long)sb->events);

    printf("Device roles (順序)     :\n");
    uint32_t n = sb->raid_disks < sb->max_dev ? sb->raid_disks : sb->max_dev;
    for (uint32_t i = 0; i < n; i++)
        printf("    dev#%-2u -> %s\n", i, role_name(roles[i], rbuf, sizeof rbuf));

    return 0;
}
