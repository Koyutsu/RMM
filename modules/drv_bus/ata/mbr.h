#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* MBR partition types we recognise */
#define PART_TYPE_EMPTY   0x00
#define PART_TYPE_FAT32   0x0B
#define PART_TYPE_FAT32_L 0x0C
#define PART_TYPE_LINUX   0x83   /* ext2/3/4 */
#define PART_TYPE_LINUX_S 0x82   /* swap */
#define PART_TYPE_GPT     0xEE   /* protective MBR for GPT */

typedef struct {
    uint8_t  status;       /* 0x80 = bootable */
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_count;
} __attribute__((packed)) mbr_part_t;

typedef struct {
    uint8_t     bootstrap[446];
    mbr_part_t  parts[4];
    uint16_t    signature;         /* 0xAA55 */
} __attribute__((packed)) mbr_t;

/* Parsed partition info */
typedef struct {
    bool     valid;
    uint8_t  type;
    uint64_t lba_start;
    uint64_t lba_count;
} part_entry_t;

/*
 * Read the MBR from bus/drive and fill entries[4].
 * Returns true if MBR signature (0xAA55) is valid.
 */
bool mbr_read(int bus, int drive, part_entry_t entries[4]);

/*
 * Find the first Linux (ext2/3/4) partition.
 * Returns -1 if none found, or the index 0-3.
 */
int mbr_find_linux(const part_entry_t entries[4]);
