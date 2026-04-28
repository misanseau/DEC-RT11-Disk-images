/*
 * test_dates.c - Self-test for the RT-11 date encoder/decoder.
 *
 * Exercises all three on-disk layouts (V3, V5, V55) and the
 * rt11_datefmt_from_sysver() SYSVER mapping.  Exits 0 on success,
 * non-zero with a diagnostic line per failure.
 *
 * Not part of the main binary -- build with:
 *   make test-dates
 */
#include <stdio.h>
#include <stdint.h>

#include "util.h"
#include "rad50.h"

static int fails = 0;

static void check(const char *tag, rt11_datefmt_t fmt,
                  int y_in, int m_in, int d_in,
                  int y_exp, int m_exp, int d_exp) {
    uint16_t w = rt11_encode_date(y_in, m_in, d_in, fmt);
    int y, m, d;
    if (rt11_decode_date(w, fmt, &y, &m, &d) != 0) {
        printf("FAIL %-18s encode=0%06o decode returned 1\n", tag, w);
        fails++;
        return;
    }
    if (y != y_exp || m != m_exp || d != d_exp) {
        printf("FAIL %-18s in=%d/%d/%d w=0%06o out=%d/%d/%d (expected %d/%d/%d)\n",
               tag, y_in, m_in, d_in, w, y, m, d, y_exp, m_exp, d_exp);
        fails++;
    } else {
        printf("OK   %-18s in=%d/%d/%d -> 0%06o -> %d/%d/%d\n",
               tag, y_in, m_in, d_in, w, y, m, d);
    }
}

int main(void) {
    /* V3 (5-bit month, year 1972-2003) */
    check("V3  apr 1975",    RT11_DATEFMT_V3,  1975, 4, 15, 1975, 4, 15);
    check("V3  oct 1980",    RT11_DATEFMT_V3,  1980,10, 15, 1980,10, 15);
    check("V3  dec 2003",    RT11_DATEFMT_V3,  2003,12, 31, 2003,12, 31);
    check("V3  clamp 2050",  RT11_DATEFMT_V3,  2050, 6, 10, 2003, 6, 10);

    /* V5 (4-bit month, year 1972-2003) */
    check("V5  apr 1985",    RT11_DATEFMT_V5,  1985, 4, 15, 1985, 4, 15);
    check("V5  dec 1999",    RT11_DATEFMT_V5,  1999,12, 31, 1999,12, 31);
    check("V5  clamp 2050",  RT11_DATEFMT_V5,  2050, 6, 10, 2003, 6, 10);

    /* V55 (4-bit month, year 1972-2099) */
    check("V55 apr 1985",    RT11_DATEFMT_V55, 1985, 4, 15, 1985, 4, 15);
    check("V55 jul 2026",    RT11_DATEFMT_V55, 2026, 7, 24, 2026, 7, 24);
    check("V55 dec 2099",    RT11_DATEFMT_V55, 2099,12, 31, 2099,12, 31);
    check("V55 clamp 2150",  RT11_DATEFMT_V55, 2150, 6, 10, 2099, 6, 10);

    /* Cross-format: a V3 date word read back as V3 must roundtrip. */
    {
        uint16_t w_v3 = rt11_encode_date(1985, 10, 15, RT11_DATEFMT_V3);
        int y, m, d;
        printf("\n-- cross-fmt roundtrip --\n");
        printf("V3  bits for 15-Oct-1985 = 0%06o\n", w_v3);
        rt11_decode_date(w_v3, RT11_DATEFMT_V3, &y, &m, &d);
        if (y == 1985 && m == 10 && d == 15) {
            printf("V3-written read as V3 : %d/%d/%d  [OK]\n", y, m, d);
        } else {
            printf("V3-written read as V3 : %d/%d/%d  [FAIL]\n", y, m, d);
            fails++;
        }
    }

    /* SYSVER -> fmt mapping. */
    {
        uint16_t v3a = rad50_encode3("V3A");
        uint16_t v05 = rad50_encode3("V05");
        uint16_t zz  = rad50_encode3("ZZZ");
        printf("\n-- sysver mapping --\n");
        printf("V3A (0%06o) -> %s\n", v3a,
               rt11_datefmt_name(rt11_datefmt_from_sysver(v3a)));
        printf("V05 (0%06o) -> %s\n", v05,
               rt11_datefmt_name(rt11_datefmt_from_sysver(v05)));
        printf("ZZZ (0%06o) -> %s\n", zz,
               rt11_datefmt_name(rt11_datefmt_from_sysver(zz)));
        printf("0    (zero) -> %s\n",
               rt11_datefmt_name(rt11_datefmt_from_sysver(0)));
        if (rt11_datefmt_from_sysver(v3a) != RT11_DATEFMT_V3)  fails++;
        if (rt11_datefmt_from_sysver(v05) != RT11_DATEFMT_V55) fails++;
        if (rt11_datefmt_from_sysver(0)   != RT11_DATEFMT_V55) fails++;
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
