#pragma once

/* Syscall 7 appends two little-endian Unix timestamps after the path buffers.
   Zero means that the snapshot predates timestamp support or has no disk entry. */
static unsigned lx_file_time(unsigned index) {
    const volatile unsigned char *field = (const volatile unsigned char *)0x443d7402 + index * 4;
    return (unsigned)field[0] | (unsigned)field[1] << 8 |
           (unsigned)field[2] << 16 | (unsigned)field[3] << 24;
}

static int leap_year(unsigned year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static void date_pair(char *text, unsigned value) {
    text[0] = '0' + value / 10;
    text[1] = '0' + value % 10;
}

/* The guest RTC is a 32-bit seconds counter. */
static void format_utc(char text[24], unsigned seconds) {
    unsigned days = seconds / 86400, year = 1970;
    while (days >= (unsigned)(365 + leap_year(year))) days -= 365 + leap_year(year++);
    static const unsigned lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned month = 0;
    while (days >= lengths[month] + (month == 1 && leap_year(year))) {
        days -= lengths[month] + (month == 1 && leap_year(year));
        ++month;
    }
    date_pair(text, year / 100);
    date_pair(text + 2, year % 100);
    text[4] = '-'; date_pair(text + 5, month + 1);
    text[7] = '-'; date_pair(text + 8, days + 1);
    text[10] = ' '; date_pair(text + 11, seconds / 3600 % 24);
    text[13] = ':'; date_pair(text + 14, seconds / 60 % 60);
    text[16] = ':'; date_pair(text + 17, seconds % 60);
    text[19] = ' '; text[20] = 'U'; text[21] = 'T'; text[22] = 'C'; text[23] = 0;
}
