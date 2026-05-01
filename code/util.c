/*
Copyright (c) 2026 Marcelo Sanseau

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
*/

/*
 * util.c - Helpers common to several modules.
 */
#include "util.h"
#include "rad50.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#if defined(_WIN32) || defined(_MSC_VER)
#  include <direct.h>
#endif

int strcieq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

size_t strlcopy(char *dst, const char *src, size_t dstsz) {
    size_t n = 0;
    if (dstsz == 0) return src ? strlen(src) : 0;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    while (src[n] && n + 1 < dstsz) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    while (src[n]) n++;
    return n;
}

void strupper(char *s) {
    if (!s) return;
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

uint16_t rd_u16(const uint8_t *buf, size_t off) {
    return (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
}

void wr_u16(uint8_t *buf, size_t off, uint16_t v) {
    buf[off]     = (uint8_t)(v & 0xFF);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xFF);
}

/* See util.h for the three RT-11 date-word layouts (V3, V5, V55). */

rt11_datefmt_t rt11_datefmt_from_sysver(uint16_t sysver) {
    /* "V3A" / "V05" are the two values ever written by DEC monitors.
     * Everything else -- including zero, which is what un-initialised
     * or RSX-11D home blocks carry -- defaults to the most permissive
     * decoder, V55. */
    if (sysver == rad50_encode3("V3A")) return RT11_DATEFMT_V3;
    if (sysver == rad50_encode3("V05")) return RT11_DATEFMT_V55;
    return RT11_DATEFMT_V55;
}

const char *rt11_datefmt_name(rt11_datefmt_t fmt) {
    switch (fmt) {
    case RT11_DATEFMT_V3:  return "V3/V4";
    case RT11_DATEFMT_V5:  return "V5.0-V5.4";
    case RT11_DATEFMT_V55: return "V5.5+";
    default:               return "?";
    }
}

uint16_t rt11_encode_date(int year, int month, int day, rt11_datefmt_t fmt) {
    int y = year - 1972;
    if (y < 0) y = 0;
    if (month < 1) month = 1;
    if (day < 1)   day   = 1;
    if (day > 31)  day   = 31;

    switch (fmt) {
    case RT11_DATEFMT_V3:
        /* 5-bit month field, no year extension: range 1972..2003. */
        if (y > 31) y = 31;
        if (month > 31) month = 31;
        return (uint16_t)(((month & 0x1F) << 10) |
                          ((day   & 0x1F) <<  5) |
                           (y     & 0x1F));

    case RT11_DATEFMT_V5:
        /* 4-bit month, bits 15-14 must remain zero: range 1972..2003. */
        if (y > 31) y = 31;
        if (month > 15) month = 15;
        return (uint16_t)(((month & 0x0F) << 10) |
                          ((day   & 0x1F) <<  5) |
                           (y     & 0x1F));

    case RT11_DATEFMT_V55:
    default:
        /* 4-bit month, 2-bit year extension: range 1972..2099. */
        if (y > 127) y = 127;
        if (month > 15) month = 15;
        return (uint16_t)((((y >> 5) & 0x3)   << 14) |
                          ((month   & 0x0F)  << 10) |
                          ((day     & 0x1F)  <<  5) |
                           (y       & 0x1F));
    }
}

int rt11_decode_date(uint16_t word, rt11_datefmt_t fmt,
                     int *y, int *m, int *d) {
    int year, month, day;
    if (word == 0) return 1;

    switch (fmt) {
    case RT11_DATEFMT_V3:
        month = (word >> 10) & 0x1F;   /* 5-bit month */
        day   = (word >>  5) & 0x1F;
        year  = 1972 + (word & 0x1F);
        break;

    case RT11_DATEFMT_V5:
    case RT11_DATEFMT_V55:
    default:
        /* Identical on decode: V5 is just V55 with bits 15-14 == 0,
         * which degrades gracefully to the same values. */
        month = (word >> 10) & 0x0F;
        day   = (word >>  5) & 0x1F;
        year  = 1972 + ((int)(((word >> 14) & 0x3) << 5) |
                        (int) (word & 0x1F));
        break;
    }

    if (y) *y = year;
    if (m) *m = month;
    if (d) *d = day;
    return 0;
}

void today_ymd(int *year, int *month, int *day) {
    time_t now = time(NULL);
    struct tm lt;
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    struct tm *p = localtime(&now);
    lt = *p;
#endif
    if (year)  *year  = lt.tm_year + 1900;
    if (month) *month = lt.tm_mon + 1;
    if (day)   *day   = lt.tm_mday;
}

int util_mkdir_p(const char *path) {
    char tmp[1024];
    size_t n;
    int rc;
    if (!path || !*path) return -1;
    /* Strip a single trailing '/' or '\\' so mkdir is happy. */
    n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    while (n > 0 && (tmp[n-1] == '/' || tmp[n-1] == '\\')) {
        tmp[--n] = '\0';
    }
    if (n == 0) return 0;   /* '/' alone -> root, fine */
#if defined(_WIN32) || defined(_MSC_VER)
    rc = _mkdir(tmp);
#else
    rc = mkdir(tmp, 0755);
#endif
    if (rc == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}
