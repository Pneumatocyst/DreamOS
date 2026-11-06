#pragma once
#include <stdint.h>

struct rtc_time { uint16_t year; uint8_t mon, day, hour, min, sec; };

void rtc_now(struct rtc_time *t);
int  rtc_weekday(const struct rtc_time *t);   /* 0 = Sunday */

extern const char *const rtc_month_names[12];
extern const char *const rtc_day_names[7];
