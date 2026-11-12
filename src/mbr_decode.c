/* mbr_decode.c — MBR プライマリ＋拡張（論理）パーティション表示
 * Usage: ./mbr_decode <disk_image_or_device>
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SECTOR_SIZE  512
#define MAX_LOGICAL  60

static const struct { uint8_t id; const char *name; } SYS_IDS[] = {
    { 0x00, "Empty"             },
    { 0x05, "Extended (CHS)"   },
    { 0x07, "NTFS/exFAT"       },
    { 0x0B, "FAT32 (CHS)"      },
    { 0x0C, "FAT32 (LBA)"      },
    { 0x0F, "Extended (LBA)"   },
    { 0x82, "Linux Swap"       },
    { 0x83, "Linux Filesystem" },
    { 0xEE, "GPT Protective"   },
    { 0xEF, "EFI System"       },
};

static const char *sys_name(uint8_t id) {
    for (size_t i = 0; i < sizeof SYS_IDS / sizeof SYS_IDS[0]; i++)
        if (SYS_IDS[i].id == id) return SYS_IDS[i].name;
    return "Unknown";
}

typedef struct __attribute__((packed)) {
    uint8_t  boot;
    uint8_t  chs_start[3];
    uint8_t  sys_id;
    uint8_t  chs_end[3];
    uint32_t lba_start;
    uint32_t sectors;
} Entry;

typedef struct __attribute__((packed)) {
    uint8_t  bootstrap[440];
    uint8_t  disk_sig[4];
    uint16_t reserved;
    Entry    entry[4];
    uint16_t signature;
} MBR;

typedef struct __attribute__((packed)) {
    uint8_t  pad[446];
    Entry    entry[4];
    uint16_t signature;
} EBR;

static void print_row(const char *label, Entry *e) {
    double mb = (double)e->sectors * SECTOR_SIZE / (1024.0 * 1024.0);
    printf("%-4s %-6s 0x%02X  %-12u %-12u %-9.1f %s\n",
           label,
           e->boot == 0x80 ? "Yes" : "No",
           e->sys_id,
           e->lba_start,
           e->sectors,
           mb,
           sys_name(e->sys_id));
}

static void walk_ebr(FILE *f, uint32_t ext_base,
                     uint32_t ebr_lba, int *num) {
    if (*num > MAX_LOGICAL) {
        fprintf(stderr, "walk_ebr: too many logical partitions\n");
        return;
    }
    if (fseek(f, (long)ebr_lba * SECTOR_SIZE, SEEK_SET) != 0) return;

    EBR ebr;
    if (fread(&ebr, sizeof ebr, 1, f) != 1) return;
    if (ebr.signature != 0xAA55) return;

    /* entry[0]: 論理パーティション（このEBR相対LBA → 絶対LBAに変換） */
    if (ebr.entry[0].sys_id != 0) {
        Entry abs = ebr.entry[0];
        abs.lba_start = ebr_lba + ebr.entry[0].lba_start;
        char label[8];
        snprintf(label, sizeof label, "L%d", *num);
        print_row(label, &abs);
        (*num)++;
    }

    /* entry[1]: 次のEBR（ext_base相対LBA → 絶対LBAに変換）、0なら終端 */
    if (ebr.entry[1].lba_start != 0)
        walk_ebr(f, ext_base, ext_base + ebr.entry[1].lba_start, num);
}

int main(int argc, char *argv[]) {
    if (argc != 2) { fprintf(stderr, "Usage: %s <disk>\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }

    MBR mbr;
    if (fread(&mbr, sizeof mbr, 1, f) != 1) {
        fprintf(stderr, "read error (file smaller than 512 bytes?)\n");
        fclose(f); return 1;
    }

    if (mbr.signature != 0xAA55) {
        fprintf(stderr, "MBR signature not found\n");
        fclose(f); return 1;
    }

    printf("Disk: %s\n", argv[1]);
    /* disk_sig は LE 格納。ディスク署名の「値」は逆順で表示する
       （fdisk の 0xXXXXXXXX 表記に一致させる） */
    printf("Disk Signature: %02x%02x%02x%02x\n",
           mbr.disk_sig[3], mbr.disk_sig[2],
           mbr.disk_sig[1], mbr.disk_sig[0]);
    printf("\n");
    printf("%-4s %-6s %-5s %-12s %-12s %-9s %s\n",
           "#", "Boot", "SysID", "LBA_Start", "Sectors", "Size_MB", "Type");
    printf("%-4s %-6s %-5s %-12s %-12s %-9s %s\n",
           "----", "------", "-----", "------------", "------------",
           "---------", "----");

    uint32_t ext_base = 0;

    for (int i = 0; i < 4; i++) {
        Entry *e = &mbr.entry[i];
        if (e->sys_id == 0 && e->lba_start == 0) continue;
        char label[8];
        snprintf(label, sizeof label, "P%d", i + 1);
        print_row(label, e);
        if (e->sys_id == 0x05 || e->sys_id == 0x0F)
            ext_base = e->lba_start;
    }

    if (ext_base != 0) {
        printf("\n--- Extended Partition (EBR chain from LBA %u) ---\n\n",
               ext_base);
        printf("%-4s %-6s %-5s %-12s %-12s %-9s %s\n",
               "#", "Boot", "SysID", "LBA_Start", "Sectors", "Size_MB", "Type");
        printf("%-4s %-6s %-5s %-12s %-12s %-9s %s\n",
               "----", "------", "-----", "------------", "------------",
               "---------", "----");
        int logical_num = 5;
        walk_ebr(f, ext_base, ext_base, &logical_num);
    }

    fclose(f);
    return 0;
}
