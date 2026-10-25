#pragma once
#include <stdint.h>

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_time_t;

void     rtc_read(rtc_time_t *t);
void     rtc_print(void);          /* prints "YYYY-MM-DD HH:MM:SS" */
int64_t  rtc_epoch_secs(void);     /* current RTC time as unix epoch (UTC) */
