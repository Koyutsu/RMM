#include "drivers/ata/mbr.h"
#include "drivers/ata/ata.h"
#include <stdint.h>
#include <stdbool.h>

bool mbr_read(int bus, int drive, part_entry_t entries[4]) {
    uint8_t sec[ATA_SECTOR_SIZE];
    if (ata_read(bus, drive, 0, 1, sec) != ATA_OK) return false;

    mbr_t *mbr = (mbr_t *)sec;
    if (mbr->signature != 0xAA55) return false;

    for (int i = 0; i < 4; i++) {
        mbr_part_t *p = &mbr->parts[i];
        entries[i].valid     = (p->type != PART_TYPE_EMPTY && p->lba_count > 0);
        entries[i].type      = p->type;
        entries[i].lba_start = p->lba_start;
        entries[i].lba_count = p->lba_count;
    }
    return true;
}

int mbr_find_linux(const part_entry_t entries[4]) {
    for (int i = 0; i < 4; i++)
        if (entries[i].valid && entries[i].type == PART_TYPE_LINUX)
            return i;
    return -1;
}
