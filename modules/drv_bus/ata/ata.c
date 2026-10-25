#include "drivers/ata/ata.h"
#include "console/print.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Port definitions */
#define ATA_REG_DATA        0   /* R/W 16-bit data          */
#define ATA_REG_ERROR       1   /* R: error                 */
#define ATA_REG_FEATURES    1   /* W: features              */
#define ATA_REG_SECCOUNT0   2
#define ATA_REG_LBA0        3
#define ATA_REG_LBA1        4
#define ATA_REG_LBA2        5
#define ATA_REG_HDDEVSEL    6
#define ATA_REG_STATUS      7   /* R: status                */
#define ATA_REG_COMMAND     7   /* W: command               */
/* 48-bit LBA high bytes (same offsets, different write) */
#define ATA_REG_SECCOUNT1   8   /* logical offset - remapped below */
#define ATA_REG_LBA3        9
#define ATA_REG_LBA4       10
#define ATA_REG_LBA5       11

/* Status bits */
#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DF    0x20
#define ATA_SR_DSC   0x10
#define ATA_SR_DRQ   0x08
#define ATA_SR_CORR  0x04
#define ATA_SR_IDX   0x02
#define ATA_SR_ERR   0x01

/* Commands */
#define ATA_CMD_READ_PIO       0x20
#define ATA_CMD_READ_PIO_EXT   0x24   /* 48-bit LBA */
#define ATA_CMD_WRITE_PIO      0x30
#define ATA_CMD_WRITE_PIO_EXT  0x34
#define ATA_CMD_IDENTIFY       0xEC

/* Bus base I/O ports */
static const uint16_t bus_io[2]   = { 0x1F0, 0x170 };
static const uint16_t bus_ctrl[2] = { 0x3F6, 0x376 };

/* Drive info */
typedef struct {
    bool     present;
    bool     lba48;
    uint64_t sector_count;
} ata_dev_t;

static ata_dev_t devs[2][2];   /* [bus][drive] */

/* Port helpers */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0,%1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0,%1" :: "a"(val), "Nd"(port));
}

/* 400ns delay via alternate status reads */
static void ata_delay(int bus) {
    for (int i = 0; i < 4; i++) inb(bus_ctrl[bus]);
}

/* Wait until BSY clears.  Returns status byte or 0xFF on timeout.
 * Budget is generous: under QEMU TCG the device can stall for many
 * milliseconds while the host schedules other work, the old fixed
 * 100k spin occasionally timed out and corrupted execs (flaky #PF). */
static uint8_t ata_wait_bsy(int bus) {
    for (uint64_t i = 0; i < 2000000; i++) {
        uint8_t s = inb(bus_io[bus] + ATA_REG_STATUS);
        if (!(s & ATA_SR_BSY)) return s;
    }
    return 0xFF;
}

/* Wait until DRQ sets (data ready). */
static uint8_t ata_wait_drq(int bus) {
    for (uint64_t i = 0; i < 2000000; i++) {
        uint8_t s = inb(bus_io[bus] + ATA_REG_STATUS);
        if (s & ATA_SR_ERR) return s;
        if (s & ATA_SR_DRQ) return s;
    }
    return 0xFF;
}

/* Select drive on bus (master=0, slave=1) */
static void ata_select(int bus, int drive) {
    outb(bus_io[bus] + ATA_REG_HDDEVSEL,
         (uint8_t)(0xA0 | (drive << 4)));
    ata_delay(bus);
}

/* IDENTIFY */
static bool ata_identify(int bus, int drive, ata_dev_t *dev) {
    dev->present      = false;
    dev->lba48        = false;
    dev->sector_count = 0;

    ata_select(bus, drive);

    /* Reset sector/LBA registers */
    outb(bus_io[bus] + ATA_REG_SECCOUNT0, 0);
    outb(bus_io[bus] + ATA_REG_LBA0, 0);
    outb(bus_io[bus] + ATA_REG_LBA1, 0);
    outb(bus_io[bus] + ATA_REG_LBA2, 0);
    outb(bus_io[bus] + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay(bus);

    /* If status 0x00 -> no drive */
    uint8_t status = inb(bus_io[bus] + ATA_REG_STATUS);
    if (status == 0x00) return false;

    /* Poll until BSY clears */
    status = ata_wait_bsy(bus);
    if (status == 0xFF) return false;

    /* Check LBA1/LBA2 - non-zero means ATAPI, not ATA */
    if (inb(bus_io[bus] + ATA_REG_LBA1) || inb(bus_io[bus] + ATA_REG_LBA2))
        return false;   /* ATAPI/SATA non-IDE */

    /* Wait for DRQ */
    status = ata_wait_drq(bus);
    if (status & ATA_SR_ERR) return false;

    /* Read 256 words of identify data */
    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(bus_io[bus] + ATA_REG_DATA);

    /* Word 83 bit 10 -> 48-bit LBA supported */
    dev->lba48 = (id[83] & (1 << 10)) != 0;

    if (dev->lba48) {
        /* Words 100-103 = 48-bit sector count */
        dev->sector_count =
            (uint64_t)id[100]       |
            ((uint64_t)id[101] << 16) |
            ((uint64_t)id[102] << 32) |
            ((uint64_t)id[103] << 48);
    } else {
        /* Words 60-61 = 28-bit sector count */
        dev->sector_count = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    }

    dev->present = (dev->sector_count > 0);
    return dev->present;
}

/* Init */
void ata_init(void) {
    for (int bus = 0; bus < 2; bus++) {
        /* Disable IRQs on this bus */
        outb(bus_ctrl[bus], 0x02);
        for (int drv = 0; drv < 2; drv++) {
            ata_identify(bus, drv, &devs[bus][drv]);
        }
    }
}

bool ata_present(int bus, int drive) {
    if (bus < 0 || bus > 1 || drive < 0 || drive > 1) return false;
    return devs[bus][drive].present;
}

uint64_t ata_sectors(int bus, int drive) {
    if (!ata_present(bus, drive)) return 0;
    return devs[bus][drive].sector_count;
}

/* Internal read/write one sector */
static int ata_pio_rw(int bus, int drive, uint64_t lba,
                       size_t count, void *buf, bool write) {
    if (!ata_present(bus, drive)) return ATA_ERR_NDEV;
    if (count == 0) return ATA_OK;

    ata_dev_t *dev = &devs[bus][drive];

    /* Validate range */
    if (lba + count > dev->sector_count) return ATA_ERR_RNG;

    uint8_t *ptr = (uint8_t *)buf;

    while (count > 0) {
        /* Transfer at most 256 sectors per command */
        size_t batch = count > 256 ? 256 : count;

        ata_wait_bsy(bus);

        if (dev->lba48) {
            /* 48-bit LBA */
            outb(bus_io[bus] + ATA_REG_HDDEVSEL,
                 (uint8_t)(0x40 | (drive << 4)));
            ata_delay(bus);

            /* High bytes first */
            outb(bus_io[bus] + ATA_REG_SECCOUNT0, (uint8_t)(batch >> 8));
            outb(bus_io[bus] + ATA_REG_LBA0, (uint8_t)(lba >> 24));
            outb(bus_io[bus] + ATA_REG_LBA1, (uint8_t)(lba >> 32));
            outb(bus_io[bus] + ATA_REG_LBA2, (uint8_t)(lba >> 40));
            /* Low bytes */
            outb(bus_io[bus] + ATA_REG_SECCOUNT0, (uint8_t)batch);
            outb(bus_io[bus] + ATA_REG_LBA0, (uint8_t)(lba));
            outb(bus_io[bus] + ATA_REG_LBA1, (uint8_t)(lba >> 8));
            outb(bus_io[bus] + ATA_REG_LBA2, (uint8_t)(lba >> 16));

            outb(bus_io[bus] + ATA_REG_COMMAND,
                 write ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_READ_PIO_EXT);
        } else {
            /* 28-bit LBA */
            outb(bus_io[bus] + ATA_REG_HDDEVSEL,
                 (uint8_t)(0xE0 | (drive << 4) | ((lba >> 24) & 0x0F)));
            ata_delay(bus);

            outb(bus_io[bus] + ATA_REG_SECCOUNT0, (uint8_t)batch);
            outb(bus_io[bus] + ATA_REG_LBA0, (uint8_t)(lba));
            outb(bus_io[bus] + ATA_REG_LBA1, (uint8_t)(lba >> 8));
            outb(bus_io[bus] + ATA_REG_LBA2, (uint8_t)(lba >> 16));

            outb(bus_io[bus] + ATA_REG_COMMAND,
                 write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);
        }

        for (size_t s = 0; s < batch; s++) {
            uint8_t st = ata_wait_drq(bus);
            if (st & ATA_SR_ERR) return ATA_ERR_IO;
            if (st == 0xFF)      return ATA_ERR_IO;

            if (write) {
                for (int w = 0; w < 256; w++) {
                    uint16_t word = (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
                    outw(bus_io[bus] + ATA_REG_DATA, word);
                    ptr += 2;
                }
                /* Flush cache */
                outb(bus_io[bus] + ATA_REG_COMMAND, 0xE7);
                ata_wait_bsy(bus);
            } else {
                for (int w = 0; w < 256; w++) {
                    uint16_t word = inw(bus_io[bus] + ATA_REG_DATA);
                    ptr[0] = (uint8_t)(word);
                    ptr[1] = (uint8_t)(word >> 8);
                    ptr += 2;
                }
            }
        }

        lba   += batch;
        count -= batch;
    }
    return ATA_OK;
}

int ata_read(int bus, int drive, uint64_t lba, size_t count, void *buf) {
    /* Retry the whole transfer up to 3 times: a transient timeout or
     * error mid-batch used to surface as a short/failed ext2 read and,
     * via execve, as the infamous flaky user-mode #PF.  Re-reading from
     * the start is idempotent for all our callers (whole-block reads). */
    int r = ATA_ERR_IO;
    for (int attempt = 0; attempt < 3; attempt++) {
        r = ata_pio_rw(bus, drive, lba, count, buf, false);
        if (r == ATA_OK) return ATA_OK;
        ata_delay(bus);
        ata_delay(bus);
    }
    return r;
}

int ata_write(int bus, int drive, uint64_t lba, size_t count, const void *buf) {
    int r = ATA_ERR_IO;
    for (int attempt = 0; attempt < 3; attempt++) {
        r = ata_pio_rw(bus, drive, lba, count, (void *)buf, true);
        if (r == ATA_OK) return ATA_OK;
        ata_delay(bus);
        ata_delay(bus);
    }
    return r;
}
