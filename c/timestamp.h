// Parse sem alocacao de timestamp ISO-8601 UTC "YYYY-MM-DDTHH:MM:SSZ" (20 chars).
#ifndef RINHA_TIMESTAMP_H
#define RINHA_TIMESTAMP_H

#include <stdint.h>

typedef struct {
    int64_t epoch;
    int hour;
    int dow_mon0; // seg=0 .. dom=6
} time_parts;

static inline int d2(const unsigned char *s, int i) {
    return (s[i] - '0') * 10 + (s[i + 1] - '0');
}
static inline int d4(const unsigned char *s, int i) {
    return (s[i] - '0') * 1000 + (s[i + 1] - '0') * 100 + (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
}

// Howard Hinnant: dias desde 1970-01-01 (proleptic Gregorian).
static inline int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline time_parts ts_parse(const unsigned char *s) {
    int64_t year = d4(s, 0), month = d2(s, 5), day = d2(s, 8);
    int hour = d2(s, 11), minute = d2(s, 14), second = d2(s, 17);
    int64_t days = days_from_civil(year, month, day);
    time_parts t;
    t.epoch = days * 86400 + (int64_t)hour * 3600 + minute * 60 + second;
    int sunday0 = (int)(((days % 7) + 4 + 7) % 7); // 1970-01-01 = quinta
    t.dow_mon0 = (sunday0 + 6) % 7;
    t.hour = hour;
    return t;
}

#endif
