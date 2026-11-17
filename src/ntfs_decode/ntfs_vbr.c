/*
 * ntfs_vbr.c  ── NTFS Volume Boot Record decoder
 *
 * Usage: ntfs_vbr <device|image> [partition_offset_bytes]
 *
 * コンパイル:
 *   cc -O2 -Wall -o ntfs_vbr ntfs_vbr.c    # Linux / macOS 共通（iconv 不要）
 *
 * 実行例:
 *   sudo ./ntfs_vbr /dev/loop1p1
 *   sudo ./ntfs_vbr HDD10MBp1p2p3-flat.vmdk 65536
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ── VBR 構造体（512 B のうち解析に必要な先頭部分） ── */
#pragma pack(push, 1)
typedef struct {
    uint8_t  jump[3];                    /* 0x00: JMP 命令 */
    uint8_t  oem_id[8];                  /* 0x03: "NTFS    " */
    uint16_t bytes_per_sector;           /* 0x0B: セクタサイズ */
    uint8_t  sectors_per_cluster;        /* 0x0D: クラスタあたりセクタ数 */
    uint8_t  reserved1[7];
    uint8_t  media_type;                 /* 0x15: 0xF8 = 固定ディスク */
    uint16_t reserved2;
    uint16_t sectors_per_track;          /* 0x18 */
    uint16_t num_heads;                  /* 0x1A */
    uint32_t hidden_sectors;             /* 0x1C */
    uint32_t reserved3;
    uint32_t reserved4;
    uint64_t total_sectors;              /* 0x28: ボリューム総セクタ数 */
    uint64_t mft_lcn;                    /* 0x30: MFT 先頭の LCN ← 重要 */
    uint64_t mft_mirror_lcn;             /* 0x38: MFT ミラーの LCN */
    int8_t   clusters_per_mft_record;    /* 0x40: MFT レコードサイズのエンコード */
    uint8_t  reserved5[3];
    int8_t   clusters_per_index_block;   /* 0x44 */
    uint8_t  reserved6[3];
    uint64_t volume_serial;              /* 0x48: ボリュームシリアル番号 */
    uint32_t checksum;                   /* 0x50 */
} NTFS_BOOT;
#pragma pack(pop)

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device|image> [partition_offset_bytes]\n",
                argv[0]);
        return 1;
    }

    /* パーティションオフセット（イメージファイル直接解析用） */
    off_t part_offset = 0;
    if (argc >= 3)
        part_offset = (off_t)strtoull(argv[2], NULL, 0);

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    NTFS_BOOT boot;
    if (pread(fd, &boot, sizeof(boot), part_offset) != (ssize_t)sizeof(boot)) {
        perror("pread"); close(fd); return 1;
    }
    close(fd);

    /* OEM ID 確認 */
    if (memcmp(boot.oem_id, "NTFS    ", 8) != 0) {
        fprintf(stderr, "Error: NTFS signature not found at offset 0x%llX\n",
                (unsigned long long)part_offset);
        return 1;
    }

    /* ── 各パラメータを計算 ── */

    uint32_t bytes_per_clus = (uint32_t)boot.bytes_per_sector
                            * (uint32_t)boot.sectors_per_cluster;

    /*
     * clusters_per_mft_record の解釈：
     *   負の値 → 2^|値| がレコードサイズ（バイト）  例: -10 → 1024 B
     *   正の値 → クラスタ数 × クラスタサイズ
     *   0      → デフォルト 1024 B
     */
    uint32_t mft_record_size;
    if (boot.clusters_per_mft_record < 0)
        mft_record_size = 1u << (uint8_t)(-boot.clusters_per_mft_record);
    else if (boot.clusters_per_mft_record > 0)
        mft_record_size = (uint32_t)boot.clusters_per_mft_record * bytes_per_clus;
    else
        mft_record_size = 1024;

    uint64_t mft_byte      = boot.mft_lcn        * bytes_per_clus;
    uint64_t mft_mirr_byte = boot.mft_mirror_lcn * bytes_per_clus;

    /* total_sectors はボリュームの最終セクタ番号（0 起点）なので +1 して容量計算 */
    uint64_t vol_bytes = (boot.total_sectors + 1) * boot.bytes_per_sector;

    /* ── 出力 ── */
    printf("Device                      : %s\n",          argv[1]);
    if (part_offset)
        printf("Partition offset            : 0x%llX (%llu B)\n",
               (unsigned long long)part_offset,
               (unsigned long long)part_offset);
    printf("Sector size                 : %u B\n",        boot.bytes_per_sector);
    printf("Sectors per cluster         : %u\n",          boot.sectors_per_cluster);
    printf("Cluster size                : %u B\n",        bytes_per_clus);
    printf("MFT record size             : %u B\n",        mft_record_size);
    printf("Total sectors               : %llu\n",        (unsigned long long)boot.total_sectors);
    printf("Volume size (approx)        : %llu B\n",      (unsigned long long)vol_bytes);
    printf("MFT LCN                     : %llu\n",        (unsigned long long)boot.mft_lcn);
    printf("MFT byte offset             : 0x%llX\n",      (unsigned long long)mft_byte);
    printf("MFT mirror LCN              : %llu\n",        (unsigned long long)boot.mft_mirror_lcn);
    printf("MFT mirror byte offset      : 0x%llX\n",      (unsigned long long)mft_mirr_byte);
    printf("Volume serial               : %016llX\n",     (unsigned long long)boot.volume_serial);

    return 0;
}

