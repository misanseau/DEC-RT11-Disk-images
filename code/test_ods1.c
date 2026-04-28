/*
 * test_ods1.c -- Drive ods1_parse_home / ods1_test against a synthetic
 * Files-11 ODS-1 home block.  Exits 0 on success.
 *
 * Build: make test-ods1
 *   or:  gcc -std=gnu99 -Wall -Wextra test_ods1.c ods1.c rad50.c util.c -o test_ods1
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ods1.h"
#include "util.h"
#include "rad50.h"

/* Field offsets reused locally so the test program is self-contained.
 * These mirror the constants in ods1.h.                              */
#define HM_IBSZ   0       /* word */
#define HM_IBLB   2       /* word - high-word of LBN, then low word   */
#define HM_FMAX   6       /* word */
#define HM_SBCL   8       /* word */
#define HM_DVTY   10      /* word */
#define HM_VLEV   12      /* word */
#define HM_VNAM   14      /* 12 bytes */
#define HM_VOWN   34      /* 12 bytes */
#define HM_INDF   496     /* "DECFILE11A  " (12 bytes) */
#define HM_CHK1_OFF   58
#define HM_CHK2_OFF   510

static void put_le16(uint8_t *p, int off, uint16_t v) {
    p[off + 0] = (uint8_t)(v & 0xff);
    p[off + 1] = (uint8_t)((v >> 8) & 0xff);
}

static uint16_t additive_checksum(const uint8_t *blk, int word_count) {
    uint16_t s = 0;
    int i;
    for (i = 0; i < word_count; i++) {
        uint16_t w = (uint16_t)blk[i*2] | ((uint16_t)blk[i*2 + 1] << 8);
        s = (uint16_t)(s + w);
    }
    return s;
}

int main(void) {
    uint8_t blk[512];
    int fails = 0;
    ods1_home_t h;

    memset(blk, 0, sizeof(blk));

    /* IBSZ = 16 (typical small index file bitmap). */
    put_le16(blk, HM_IBSZ, 16);

    /* IBLB = LBN where the index file bitmap starts.  Files-11 stores
     * it "high-word-first": high word at offset 2, low word at offset 4.
     * For LBN 12 this is high=0, low=12.                              */
    put_le16(blk, HM_IBLB + 0, 0);    /* high word */
    put_le16(blk, HM_IBLB + 2, 12);   /* low word  */

    put_le16(blk, HM_FMAX, 1024);     /* max files                     */
    put_le16(blk, HM_SBCL, 1);        /* storage bitmap cluster (==1)  */
    put_le16(blk, HM_DVTY, 0);        /* device type            (==0)  */
    put_le16(blk, HM_VLEV, 0x0101);   /* ODS-1 structure level         */

    /* Volume name "TESTVOL" padded to 12 bytes. */
    memcpy(blk + HM_VNAM, "TESTVOL     ", 12);
    /* Owner UIC. */
    memcpy(blk + HM_VOWN, "[001,001]   ", 12);
    /* Format signature "DECFILE11A  " (12 bytes). */
    memcpy(blk + HM_INDF, "DECFILE11A  ", 12);

    /* First checksum: sum of words 0..28, store at word 29 (byte 58). */
    put_le16(blk, HM_CHK1_OFF, additive_checksum(blk, 29));
    /* Second checksum: sum of words 0..254, store at word 255 (byte 510). */
    put_le16(blk, HM_CHK2_OFF, additive_checksum(blk, 255));

    /* --- Run ods1_parse_home + ods1_test on it. --- */
    ods1_parse_home(blk, &h);

    printf("=== Synthetic ODS-1 home block ===\n");
    printf(" IBSZ=%u IBLB=%u FMAX=%u VLEV=0x%04x\n",
           h.ibsz, h.iblb, h.fmax, h.vlev);
    printf(" chk1 stored=0x%04x computed=0x%04x ok=%d\n",
           h.chk1, h.chk1_computed, h.chk1_ok);
    printf(" chk2 stored=0x%04x computed=0x%04x ok=%d\n",
           h.chk2, h.chk2_computed, h.chk2_ok);
    printf(" sig_ok=%d vlev_ok=%d sbcl_ok=%d dvty_ok=%d\n",
           h.sig_ok, h.vlev_ok, h.sbcl_ok, h.dvty_ok);
    printf(" Volume name \"%s\"\n", h.vnam);
    printf(" Format sig  \"%s\"\n", h.indf);

    if (!h.chk1_ok)    { printf("FAIL chk1\n");        fails++; }
    if (!h.chk2_ok)    { printf("FAIL chk2\n");        fails++; }
    if (!h.sig_ok)     { printf("FAIL sig\n");         fails++; }
    if (!h.vlev_ok)    { printf("FAIL vlev\n");        fails++; }
    if (!h.sbcl_ok)    { printf("FAIL sbcl\n");        fails++; }
    if (!h.dvty_ok)    { printf("FAIL dvty\n");        fails++; }
    if (h.ibsz != 16)  { printf("FAIL ibsz\n");        fails++; }
    if (h.iblb != 12)  { printf("FAIL iblb=%u\n", h.iblb); fails++; }
    if (h.fmax != 1024){ printf("FAIL fmax\n");        fails++; }

    /* Positive: ods1_test() at level 2 must PASS. */
    {
        ods1_test_t t = ods1_test(2, /*volume_blocks*/ 4096, blk);
        printf("\n ods1_test level=2 -> result=%d (PASS=%d) reached=%d\n",
               t.result, ODS1_TEST_PASS, t.level_reached);
        if (t.reason[0]) printf(" reason: %s\n", t.reason);
        if (t.result != ODS1_TEST_PASS) { printf("FAIL ods1_test\n"); fails++; }
    }

    /* Negative: trash the signature, ods1_test() must FAIL. */
    {
        uint8_t bad[512];
        ods1_test_t t;
        memcpy(bad, blk, 512);
        memset(bad + HM_INDF, 'X', 12);
        put_le16(bad, HM_CHK1_OFF, 0);
        put_le16(bad, HM_CHK2_OFF, 0);
        put_le16(bad, HM_CHK1_OFF, additive_checksum(bad, 29));
        put_le16(bad, HM_CHK2_OFF, additive_checksum(bad, 255));
        t = ods1_test(2, 4096, bad);
        printf("\n negative (no DECFILE11A) -> result=%d (FAIL=%d)\n",
               t.result, ODS1_TEST_FAIL);
        if (t.reason[0]) printf(" reason: %s\n", t.reason);
        if (t.result != ODS1_TEST_FAIL) { printf("FAIL negative-sig\n"); fails++; }
    }

    /* Pretty-printer smoke test. */
    printf("\n");
    ods1_print_home(stdout, &h);

    printf("\n%s (%d failure%s)\n",
           fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
