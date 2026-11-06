/* The real-time clock: a battery-backed chip (the "CMOS") that keeps the date
   while the machine is off. We read it through ports 0x70/0x71. */
#include "drivers/rtc.h"
#include "arch/io.h"
#include <stdbool.h>

const char *const rtc_month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};
const char *const rtc_day_names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static void read_raw(struct rtc_time *t) {
    while (cmos(0x0a) & 0x80) {}          /* wait out an update in progress */
    t->sec  = cmos(0x00);
    t->min  = cmos(0x02);
    t->hour = cmos(0x04);
    t->day  = cmos(0x07);
    t->mon  = cmos(0x08);
    t->year = cmos(0x09);
}

static uint8_t bcd(uint8_t v) { return (v & 0x0f) + (v >> 4) * 10; }

void rtc_now(struct rtc_time *t) {
    /* Reading the clock is "write a register number, then read its value".
       If another task did the same in between, we'd read the wrong register. */
    uint64_t flags = irq_save();
    struct rtc_time a, b;
    do { read_raw(&a); read_raw(&b); }    /* read twice until two reads agree */
    while (a.sec != b.sec || a.min != b.min || a.hour != b.hour || a.day != b.day);

    uint8_t status = cmos(0x0b);
    bool pm = a.hour & 0x80;
    a.hour &= 0x7f;
    if (!(status & 0x04)) {               /* values are BCD unless bit 2 says binary */
        a.sec = bcd(a.sec); a.min = bcd(a.min); a.hour = bcd(a.hour);
        a.day = bcd(a.day); a.mon = bcd(a.mon); a.year = bcd(a.year);
    }
    if (!(status & 0x02) && pm) a.hour = (a.hour + 12) % 24;   /* 12-hour mode */
    a.year += 2000;
    *t = a;
    irq_restore(flags);
}

int rtc_weekday(const struct rtc_time *t) {   /* Sakamoto's method */
    static const int k[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = t->year - (t->mon < 3);
    return (y + y / 4 - y / 100 + y / 400 + k[t->mon - 1] + t->day) % 7;
}
