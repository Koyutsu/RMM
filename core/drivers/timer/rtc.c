#include "drivers/timer/rtc.h"
#include "drivers/pci/io.h"
#include "console/display.h"
#include <stdint.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outportb(CMOS_ADDR, reg & 0x7F);   /* keep NMI disable bit clear */
    return inportb(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

static int rtc_updating(void) {
    outportb(CMOS_ADDR, 0x0A);
    return inportb(CMOS_DATA) & 0x80;
}

void rtc_read(rtc_time_t *t) {
    /* Wait for RTC update to finish */
    while (rtc_updating()) {}

    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day  = cmos_read(0x07);
    uint8_t mon  = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t cent = cmos_read(0x32);
    uint8_t regB = cmos_read(0x0B);

    /* Convert BCD -> binary unless bit 2 of register B is set (binary mode) */
    if (!(regB & 0x04)) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F) | (hour & 0x80);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
        if (cent) cent = bcd_to_bin(cent);
    }

    /* 12-hour PM bit -> 24-hour */
    if (!(regB & 0x02) && (hour & 0x80)) {
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    } else {
        hour &= 0x7F;
    }

    t->second = sec;
    t->minute = min;
    t->hour   = hour;
    t->day    = day;
    t->month  = mon;

    /* Year: try century register first, fall back to 2000 assumption */
    if (cent >= 20 && cent <= 21) {
        /* Reasonable century value (20xx or 21xx) */
        t->year = (uint16_t)((uint16_t)cent * 100 + year);
    } else {
        /* QEMU / many BIOSes leave cent=0 or garbage, use 2000+year */
        t->year = (uint16_t)(2000 + year);
    }
}

static void print2(uint8_t v) {
    display_putchar((char)('0' + v / 10));
    display_putchar((char)('0' + v % 10));
}

void rtc_print(void) {
    rtc_time_t t;
    rtc_read(&t);
    display_printf("%u", (unsigned)t.year);
    display_putchar('-'); print2(t.month);
    display_putchar('-'); print2(t.day);
    display_puts("  ");
    print2(t.hour);   display_putchar(':');
    print2(t.minute); display_putchar(':');
    print2(t.second);
}

/* Unix epoch conversion
 * days_from_civil (Hinnant) converts Y/M/D to a day count relative to
 * 1970-01-01.  The RTC is interpreted as UTC - RMM ships no TZ
 * database, so userspace sees wall time in UTC (like a Linux box with
 * TZ=UTC). */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);            /* [0,399] */
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t rtc_epoch_secs(void) {
    rtc_time_t t;
    rtc_read(&t);
    int64_t days = days_from_civil((int)t.year, t.month, t.day);
    return days * 86400 + (int64_t)t.hour * 3600 +
           (int64_t)t.minute * 60 + t.second;
}
