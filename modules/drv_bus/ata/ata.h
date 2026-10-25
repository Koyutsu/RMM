#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * ata_pio - ATA-2 PIO mode driver
 *
 * Supports 28-bit and 48-bit LBA read/write on the primary IDE bus.
 * Works on real hardware (PATA, legacy SATA in IDE-compat mode) and
 * in QEMU (-drive if=ide).
 *
 * Only polling (no DMA, no IRQ).  Suitable for boot-time disk access
 * before any interrupt infrastructure beyond the keyboard is needed.
 *
 * Bus layout:
 *   Primary   I/O: 0x1F0-0x1F7   Control: 0x3F6
 *   Secondary I/O: 0x170-0x177   Control: 0x376
 */

/* Drive selection */
#define ATA_PRIMARY   0
#define ATA_SECONDARY 1
#define ATA_MASTER    0
#define ATA_SLAVE     1

/* Sector size */
#define ATA_SECTOR_SIZE  512

/* Error codes (negative) */
#define ATA_OK        0
#define ATA_ERR_NDEV -1   /* no device present */
#define ATA_ERR_IO   -2   /* I/O error / DRQ timeout */
#define ATA_ERR_RNG  -3   /* LBA out of range */

/* Probe and identify all ATA devices.  Call once at boot. */
void ata_init(void);

/* Return true if the given bus+drive exists and is ready. */
bool ata_present(int bus, int drive);

/* Size of the drive in 512-byte sectors (0 if not present). */
uint64_t ata_sectors(int bus, int drive);

/*
 * Read `count` sectors starting at LBA `lba` from bus/drive into buf.
 * buf must be at least count * 512 bytes.
 * Returns ATA_OK or negative error.
 */
int ata_read(int bus, int drive, uint64_t lba, size_t count, void *buf);

/*
 * Write `count` sectors.  Same convention.
 */
int ata_write(int bus, int drive, uint64_t lba, size_t count, const void *buf);
