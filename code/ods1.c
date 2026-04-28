/*
 * ods1.c - Files-11 ODS-1 decoders.  Read-only, no I/O of its own.
 */
#include "ods1.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Local helper: read a 16-bit little-endian word from a buffer at byte
 * offset off.  We can't reuse rd_u16() from util.c because it takes a
 * size_t off and we don't want a dependency cycle around the header. */
static uint16_t le16(const uint8_t *buf, size_t off) {
    return (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
}

/* Strip trailing whitespace / NUL from a fixed-width on-disk string and
 * NUL-terminate it inside a destination buffer.  src is exactly src_len
 * bytes.  dst must have at least src_len + 1 bytes. */
static void copy_padded(char *dst, const uint8_t *src, size_t src_len) {
    size_t i;
    size_t end = 0;
    for (i = 0; i < src_len; i++) {
        unsigned char c = src[i];
        dst[i] = (char)c;
        if (c != 0 && c != ' ') end = i + 1;
    }
    dst[end] = '\0';
}

/* ---------------------------------------------------------------------
 * Checksums
 * --------------------------------------------------------------------- */

uint16_t ods1_checksum(const uint8_t *buf, size_t checksum_off) {
    uint32_t sum = 0;
    size_t   off;
    for (off = 0; off + 1 < checksum_off; off += 2) {
        sum += le16(buf, off);
    }
    return (uint16_t)(sum & 0xFFFFu);
}

/* ---------------------------------------------------------------------
 * RAD50 (3 chars / word).  Same alphabet as RT-11.
 * --------------------------------------------------------------------- */

static const char ODS1_RAD50_CHARS[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789";
/*   0                                       39 */
/* index 29 ('?') is the unused slot, decoded as '%' to match FSX. */

static void rad50_decode_word(uint16_t w, char out[3]) {
    out[0] = ODS1_RAD50_CHARS[(w / (050 * 050)) % 050];
    out[1] = ODS1_RAD50_CHARS[(w / 050) % 050];
    out[2] = ODS1_RAD50_CHARS[ w % 050];
}

void ods1_rad50_decode_name(uint16_t w0, uint16_t w1, uint16_t w2,
                            char out[10]) {
    rad50_decode_word(w0, out + 0);
    rad50_decode_word(w1, out + 3);
    rad50_decode_word(w2, out + 6);
    out[9] = '\0';
    /* Trim trailing spaces for a friendlier display. */
    while (out[0] && out[strlen(out) - 1] == ' ') {
        out[strlen(out) - 1] = '\0';
    }
}

void ods1_rad50_decode_type(uint16_t w, char out[4]) {
    rad50_decode_word(w, out);
    out[3] = '\0';
    while (out[0] && out[strlen(out) - 1] == ' ') {
        out[strlen(out) - 1] = '\0';
    }
}

/* ---------------------------------------------------------------------
 * Home block parser
 * --------------------------------------------------------------------- */

int ods1_parse_home(const uint8_t *buf, ods1_home_t *out) {
    memset(out, 0, sizeof(*out));

    out->ibsz = le16(buf, ODS1_HB_IBSZ_OFF);
    /* IBLB is stored hi-word first (ODS-1 quirk), unlike everything
     * else on disk which is little-endian.  See spec & files11.txt. */
    out->iblb = ((uint32_t)le16(buf, ODS1_HB_IBLB_HI_OFF) << 16) |
                 (uint32_t)le16(buf, ODS1_HB_IBLB_LO_OFF);
    out->fmax = le16(buf, ODS1_HB_FMAX_OFF);
    out->sbcl = le16(buf, ODS1_HB_SBCL_OFF);
    out->dvty = le16(buf, ODS1_HB_DVTY_OFF);
    out->vlev = le16(buf, ODS1_HB_VLEV_OFF);

    copy_padded(out->vnam, buf + ODS1_HB_VNAM_OFF, 12);

    out->vown = le16(buf, ODS1_HB_VOWN_OFF);
    out->vpro = le16(buf, ODS1_HB_VPRO_OFF);
    out->vcha = le16(buf, ODS1_HB_VCHA_OFF);
    out->dfpr = le16(buf, ODS1_HB_DFPR_OFF);
    out->wisz = buf[ODS1_HB_WISZ_OFF];
    out->fiex = buf[ODS1_HB_FIEX_OFF];
    out->lruc = buf[ODS1_HB_LRUC_OFF];

    /* VDAT is 13 ASCII bytes "DDMMMYYHHMMSS" (no terminator on disk). */
    {
        size_t i;
        for (i = 0; i < 13; i++) out->vdat[i] = (char)buf[ODS1_HB_VDAT_OFF + i];
        out->vdat[13] = '\0';
    }

    copy_padded(out->indn, buf + ODS1_HB_INDN_OFF, 12);
    copy_padded(out->indo, buf + ODS1_HB_INDO_OFF, 12);
    copy_padded(out->indf, buf + ODS1_HB_INDF_OFF, 12);

    /* Checksums */
    out->chk1          = le16(buf, ODS1_HB_CHK1_OFF);
    out->chk1_computed = ods1_checksum(buf, ODS1_HB_CHK1_OFF);
    out->chk2          = le16(buf, ODS1_HB_CHK2_OFF);
    out->chk2_computed = ods1_checksum(buf, ODS1_HB_CHK2_OFF);

    out->chk1_ok = (out->chk1_computed == out->chk1) && (out->chk1 != 0);
    out->chk2_ok = (out->chk2_computed == out->chk2) && (out->chk2 != 0);

    /* Signature: bytes 496..507 must be exactly "DECFILE11A  " (12 B). */
    out->sig_ok = (memcmp(buf + ODS1_HB_INDF_OFF,
                          ODS1_HB_FORMAT_SIG, 12) == 0);

    out->vlev_ok = (out->vlev == ODS1_HB_VLEV_VALUE_V1) ||
                   (out->vlev == ODS1_HB_VLEV_VALUE_V12);
    out->sbcl_ok = (out->sbcl == 1);
    out->dvty_ok = (out->dvty == 0);

    /* "Looks like ODS-1" requires both signature and second checksum
     * (the more comprehensive one) to be valid.  Some images have only
     * a partially-initialised first 58 bytes -- we accept those too. */
    return out->sig_ok && out->chk2_ok;
}

/* ---------------------------------------------------------------------
 * File header parser
 * --------------------------------------------------------------------- */

int ods1_parse_fh(const uint8_t *buf, ods1_fh_t *out) {
    uint8_t  idof, mpof;
    size_t   id_off;
    size_t   map_off;

    memset(out, 0, sizeof(*out));

    idof = buf[ODS1_FH_IDOF_OFF];
    mpof = buf[ODS1_FH_MPOF_OFF];
    out->idof = idof;
    out->mpof = mpof;

    out->fnum = le16(buf, ODS1_FH_FNUM_OFF);
    out->fseq = le16(buf, ODS1_FH_FSEQ_OFF);
    out->flev = le16(buf, ODS1_FH_FLEV_OFF);
    out->fown = le16(buf, ODS1_FH_FOWN_OFF);
    out->fpro = le16(buf, ODS1_FH_FPRO_OFF);
    out->fcha = le16(buf, ODS1_FH_FCHA_OFF);

    /* User attribute area (FCS-11) at H.UFAT (offset 14, 32 bytes). */
    out->rtyp = buf[ODS1_FH_UFAT_OFF + ODS1_UFAT_RTYP_OFF];
    out->ratt = buf[ODS1_FH_UFAT_OFF + ODS1_UFAT_RATT_OFF];
    out->rsiz = le16(buf, ODS1_FH_UFAT_OFF + ODS1_UFAT_RSIZ_OFF);
    /* HIBK and EFBK are stored as 32-bit values in big/little quirky
     * order: high-word first then low-word, similar to IBLB. */
    out->hibk = ((uint32_t)le16(buf, ODS1_FH_UFAT_OFF + ODS1_UFAT_HIBK_OFF)     << 16) |
                 (uint32_t)le16(buf, ODS1_FH_UFAT_OFF + ODS1_UFAT_HIBK_OFF + 2);
    out->efbk = ((uint32_t)le16(buf, ODS1_FH_UFAT_OFF + ODS1_UFAT_EFBK_OFF)     << 16) |
                 (uint32_t)le16(buf, ODS1_FH_UFAT_OFF + ODS1_UFAT_EFBK_OFF + 2);
    out->ffby = le16(buf, ODS1_FH_UFAT_OFF + ODS1_UFAT_FFBY_OFF);

    /* Validate offsets up front: both are in WORDS, and the file
     * header is exactly 256 words long (last word is the checksum). */
    out->offsets_ok = (idof > 0) && (mpof >= idof) && (mpof < 0xFD);

    /* Ident area */
    id_off = (size_t)idof * 2u;
    if (out->offsets_ok && id_off + ODS1_ID_EXDT_OFF + 7 <= ODS1_BLOCK_SIZE) {
        uint16_t fn0 = le16(buf, id_off + ODS1_ID_FNAM_OFF + 0);
        uint16_t fn1 = le16(buf, id_off + ODS1_ID_FNAM_OFF + 2);
        uint16_t fn2 = le16(buf, id_off + ODS1_ID_FNAM_OFF + 4);
        uint16_t ft  = le16(buf, id_off + ODS1_ID_FTYP_OFF);
        ods1_rad50_decode_name(fn0, fn1, fn2, out->fname);
        ods1_rad50_decode_type(ft, out->ftype);
        out->fver = (int16_t)le16(buf, id_off + ODS1_ID_FVER_OFF);
        out->rvno = le16(buf, id_off + ODS1_ID_RVNO_OFF);
        memcpy(out->rvdt, buf + id_off + ODS1_ID_RVDT_OFF, 7); out->rvdt[7] = '\0';
        memcpy(out->rvti, buf + id_off + ODS1_ID_RVTI_OFF, 6); out->rvti[6] = '\0';
        memcpy(out->crdt, buf + id_off + ODS1_ID_CRDT_OFF, 7); out->crdt[7] = '\0';
        memcpy(out->crti, buf + id_off + ODS1_ID_CRTI_OFF, 6); out->crti[6] = '\0';
        memcpy(out->exdt, buf + id_off + ODS1_ID_EXDT_OFF, 7); out->exdt[7] = '\0';
    }

    /* Map area */
    map_off = (size_t)mpof * 2u;
    if (out->offsets_ok && map_off + ODS1_MAP_RTRV_OFF <= ODS1_BLOCK_SIZE) {
        out->m_esqn = buf[map_off + ODS1_MAP_ESQN_OFF];
        out->m_ervn = buf[map_off + ODS1_MAP_ERVN_OFF];
        out->m_efnu = le16(buf, map_off + ODS1_MAP_EFNU_OFF);
        out->m_efsq = le16(buf, map_off + ODS1_MAP_EFSQ_OFF);
        out->m_ctsz = buf[map_off + ODS1_MAP_CTSZ_OFF];
        out->m_lbsz = buf[map_off + ODS1_MAP_LBSZ_OFF];
        out->m_use  = buf[map_off + ODS1_MAP_USE_OFF];
        out->m_max  = buf[map_off + ODS1_MAP_MAX_OFF];
    }

    out->cksm          = le16(buf, ODS1_FH_CKSM_OFF);
    out->cksm_computed = ods1_checksum(buf, ODS1_FH_CKSM_OFF);
    out->cksm_ok       = (out->cksm == out->cksm_computed) && (out->cksm != 0);
    out->flev_ok       = (out->flev == ODS1_FH_FLEV_VALUE);

    return out->cksm_ok && out->flev_ok && out->offsets_ok;
}

/* ---------------------------------------------------------------------
 * Map area walking
 * --------------------------------------------------------------------- */

int ods1_walk_map(const uint8_t *buf, const ods1_fh_t *fh,
                  ods1_retr_cb cb, void *arg) {
    size_t   map_off = (size_t)fh->mpof * 2u;
    size_t   p       = map_off + ODS1_MAP_RTRV_OFF;
    size_t   end_p   = p + (size_t)fh->m_use * 2u;
    uint32_t idx     = 0;

    if (end_p > ODS1_BLOCK_SIZE) return -1;

    while (p < end_p) {
        ods1_retr_t r;

        if (fh->m_ctsz == 1 && fh->m_lbsz == 3) {
            /* Format 1 (the common one): byte LBN_hi, byte count-1,
             * word LBN_lo (little-endian). */
            uint8_t  hi    = buf[p++];
            uint8_t  ct1   = buf[p++];
            uint16_t lo;
            if (p + 2 > end_p) return -1;
            lo = le16(buf, p); p += 2;
            r.lbn   = ((uint32_t)hi << 16) | lo;
            r.count = (uint32_t)ct1 + 1u;
        } else if (fh->m_ctsz == 2 && fh->m_lbsz == 2) {
            /* Format 2: word count, word LBN. */
            uint16_t ct, lbn;
            if (p + 4 > end_p) return -1;
            ct  = le16(buf, p); p += 2;
            lbn = le16(buf, p); p += 2;
            r.lbn   = lbn;
            r.count = (uint32_t)ct + 1u;
        } else if (fh->m_ctsz == 2 && fh->m_lbsz == 4) {
            /* Format 3: word count, word LBN_hi, word LBN_lo. */
            uint16_t ct, hi, lo;
            if (p + 6 > end_p) return -1;
            ct = le16(buf, p); p += 2;
            hi = le16(buf, p); p += 2;
            lo = le16(buf, p); p += 2;
            r.lbn   = ((uint32_t)hi << 16) | lo;
            r.count = (uint32_t)ct + 1u;
        } else {
            return -1;     /* unknown / illegal format */
        }

        if (cb) cb(arg, &r, idx);
        idx++;
    }

    return (int)idx;
}

/* ---------------------------------------------------------------------
 * File header LBN for files 1..16 (no index file walk required)
 * --------------------------------------------------------------------- */

uint32_t ods1_fh_lbn(const ods1_home_t *hb, uint16_t fnum) {
    if (fnum == 0 || fnum > 16) return 0;
    return hb->iblb + hb->ibsz + (uint32_t)(fnum - 1);
}

/* ---------------------------------------------------------------------
 * Directory entry parser (16 bytes per entry)
 *
 * Layout (from FSX / Mansfield ch.1):
 *   word 0   file number       (LE)
 *   word 1   file sequence #   (LE)
 *   word 2   relative volume # (LE, almost always 0 on single-vol packs)
 *   words 3-5  filename, 9 chars in 3 RAD-50 words
 *   word 6   filetype, 3 chars in 1 RAD-50 word
 *   word 7   version (signed)
 * --------------------------------------------------------------------- */

int ods1_parse_dirent(const uint8_t *buf, ods1_dirent_t *out) {
    uint16_t w0, w1, w2, wt;
    if (!buf || !out) return 0;
    memset(out, 0, sizeof(*out));

    out->fnum = (uint16_t)(buf[ODS1_DE_FNUM_OFF] |
                           ((uint16_t)buf[ODS1_DE_FNUM_OFF+1] << 8));
    out->fseq = (uint16_t)(buf[ODS1_DE_FSEQ_OFF] |
                           ((uint16_t)buf[ODS1_DE_FSEQ_OFF+1] << 8));
    out->frvn = (uint16_t)(buf[ODS1_DE_FRVN_OFF] |
                           ((uint16_t)buf[ODS1_DE_FRVN_OFF+1] << 8));

    if (out->fnum == 0) {
        /* Empty / unused slot.  Leave name/type/version zeroed. */
        out->name[0] = '\0';
        out->type[0] = '\0';
        return 0;
    }

    w0 = (uint16_t)(buf[ODS1_DE_NAME_OFF]   |
                    ((uint16_t)buf[ODS1_DE_NAME_OFF+1] << 8));
    w1 = (uint16_t)(buf[ODS1_DE_NAME_OFF+2] |
                    ((uint16_t)buf[ODS1_DE_NAME_OFF+3] << 8));
    w2 = (uint16_t)(buf[ODS1_DE_NAME_OFF+4] |
                    ((uint16_t)buf[ODS1_DE_NAME_OFF+5] << 8));
    wt = (uint16_t)(buf[ODS1_DE_TYPE_OFF]   |
                    ((uint16_t)buf[ODS1_DE_TYPE_OFF+1] << 8));
    ods1_rad50_decode_name(w0, w1, w2, out->name);
    ods1_rad50_decode_type(wt, out->type);

    out->version = (int16_t)(buf[ODS1_DE_VERS_OFF] |
                             ((uint16_t)buf[ODS1_DE_VERS_OFF+1] << 8));
    return 1;
}

/* ---------------------------------------------------------------------
 * FSX-style auto-detection (levels 0..2)
 * --------------------------------------------------------------------- */

ods1_test_t ods1_test(int level, uint32_t volume_blocks,
                      const uint8_t *home_block) {
    ods1_test_t t;
    memset(&t, 0, sizeof(t));
    t.volume_size_blocks = -1;
    t.result             = ODS1_TEST_FAIL;
    t.level_reached      = -1;

    /* Level 0: block size is implicit -- ODS-1 always uses 512 byte
     * blocks.  Always passes here, since callers wouldn't be calling
     * this routine with anything else.                                */
    t.level_reached = 0;
    if (level == 0) { t.result = ODS1_TEST_PASS; return t; }

    /* Level 1: needs at least the boot block.  No size assertion. */
    if (volume_blocks != 0 && volume_blocks < 1) {
        snprintf(t.reason, sizeof(t.reason),
                 "volume too small to contain boot block");
        return t;
    }
    t.level_reached = 1;
    if (level == 1) { t.result = ODS1_TEST_PASS; return t; }

    /* Level 2: home block.  Requires a buffer to inspect. */
    if (home_block == NULL) {
        snprintf(t.reason, sizeof(t.reason),
                 "home block buffer not provided for level >= 2");
        return t;
    }
    if (volume_blocks != 0 && volume_blocks < 2) {
        snprintf(t.reason, sizeof(t.reason),
                 "volume too small to contain home block");
        return t;
    }

    {
        ods1_home_t h;
        ods1_parse_home(home_block, &h);

        if (!h.chk1_ok) {
            snprintf(t.reason, sizeof(t.reason),
                     "home block first checksum invalid (got 0x%04x, expected 0x%04x)",
                     h.chk1, h.chk1_computed);
            return t;
        }
        if (!h.chk2_ok) {
            snprintf(t.reason, sizeof(t.reason),
                     "home block second checksum invalid (got 0x%04x, expected 0x%04x)",
                     h.chk2, h.chk2_computed);
            return t;
        }
        if (!h.sig_ok) {
            snprintf(t.reason, sizeof(t.reason),
                     "home block format signature missing (\"DECFILE11A\")");
            return t;
        }
        if (!h.vlev_ok) {
            snprintf(t.reason, sizeof(t.reason),
                     "home block VLEV not 0x0101/0x0102 (got 0x%04x)",
                     h.vlev);
            return t;
        }
        if (h.fmax < 16) {
            snprintf(t.reason, sizeof(t.reason),
                     "home block FMAX < 16 (got %u)", h.fmax);
            return t;
        }
        if (!h.sbcl_ok) {
            snprintf(t.reason, sizeof(t.reason),
                     "home block SBCL != 1 (got %u)", h.sbcl);
            return t;
        }
        if (!h.dvty_ok) {
            snprintf(t.reason, sizeof(t.reason),
                     "home block DVTY != 0 (got %u)", h.dvty);
            return t;
        }
        if (h.ibsz == 0) {
            snprintf(t.reason, sizeof(t.reason),
                     "home block IBSZ == 0");
            return t;
        }
        if (h.iblb < 2) {
            snprintf(t.reason, sizeof(t.reason),
                     "home block IBLB < 2 (got %u)", h.iblb);
            return t;
        }
    }

    t.level_reached = 2;
    t.result        = ODS1_TEST_PASS;
    /* Volume size isn't recorded in the home block.  Honest answer: -1. */
    t.volume_size_blocks = -1;
    return t;
}

/* ---------------------------------------------------------------------
 * Pretty-printer for EXAM
 * --------------------------------------------------------------------- */

void ods1_print_home(void *fp_void, const ods1_home_t *h) {
    FILE *fp = (FILE *)fp_void;
    if (!fp) fp = stdout;

    fprintf(fp, "\n  -- Interpreted as Files-11 ODS-1 home block --\n");
    fprintf(fp, "    [%04o] Volume name (H.VNAM)     : \"%s\"\n",
            ODS1_HB_VNAM_OFF, h->vnam);
    fprintf(fp, "    [%04o] Volume name (H.INDN)     : \"%s\"\n",
            ODS1_HB_INDN_OFF, h->indn);
    fprintf(fp, "    [%04o] Owner       (H.INDO)     : \"%s\"\n",
            ODS1_HB_INDO_OFF, h->indo);
    fprintf(fp, "    [%04o] Format sig  (H.INDF)     : \"%s\"  %s\n",
            ODS1_HB_INDF_OFF, h->indf, h->sig_ok ? "[OK]" : "[BAD]");
    fprintf(fp, "    [%04o] Volume struct level VLEV : 0x%04x  %s\n",
            ODS1_HB_VLEV_OFF, h->vlev, h->vlev_ok ? "[OK]" : "[unexpected]");
    fprintf(fp, "    [%04o] Volume creation H.VDAT   : \"%s\"\n",
            ODS1_HB_VDAT_OFF, h->vdat);
    fprintf(fp, "    [%04o] Owner UIC   (H.VOWN)     : 0%06o  (group=%03o user=%03o)\n",
            ODS1_HB_VOWN_OFF, h->vown,
            (h->vown >> 8) & 0xff, h->vown & 0xff);
    fprintf(fp, "    [%04o] Volume protection VPRO   : 0%06o\n",
            ODS1_HB_VPRO_OFF, h->vpro);
    fprintf(fp, "    [%04o] Volume characteristics   : 0%06o\n",
            ODS1_HB_VCHA_OFF, h->vcha);
    fprintf(fp, "    [%04o] Default file protection  : 0%06o\n",
            ODS1_HB_DFPR_OFF, h->dfpr);
    fprintf(fp, "    [%04o] Default window/extend    : %u / %u  (LRUC=%u)\n",
            ODS1_HB_WISZ_OFF, h->wisz, h->fiex, h->lruc);
    fprintf(fp, "    [%04o] Index file bitmap size   : %u block%s\n",
            ODS1_HB_IBSZ_OFF, h->ibsz, h->ibsz == 1 ? "" : "s");
    fprintf(fp, "    [%04o] Index file bitmap LBN    : %u  (hi at %04o, lo at %04o)\n",
            ODS1_HB_IBLB_HI_OFF, h->iblb,
            ODS1_HB_IBLB_HI_OFF, ODS1_HB_IBLB_LO_OFF);
    fprintf(fp, "    [%04o] Maximum number of files  : %u\n",
            ODS1_HB_FMAX_OFF, h->fmax);
    fprintf(fp, "    [%04o] Storage bitmap cluster   : %u  %s\n",
            ODS1_HB_SBCL_OFF, h->sbcl, h->sbcl_ok ? "[OK]" : "[expected 1]");
    fprintf(fp, "    [%04o] Disk device type         : %u  %s\n",
            ODS1_HB_DVTY_OFF, h->dvty, h->dvty_ok ? "[OK]" : "[expected 0]");
    fprintf(fp, "    [%04o] First checksum  (CHK1)   : 0x%04x  computed 0x%04x  %s\n",
            ODS1_HB_CHK1_OFF, h->chk1, h->chk1_computed,
            h->chk1_ok ? "[OK]" : "[MISMATCH]");
    fprintf(fp, "    [%04o] Second checksum (CHK2)   : 0x%04x  computed 0x%04x  %s\n",
            ODS1_HB_CHK2_OFF, h->chk2, h->chk2_computed,
            h->chk2_ok ? "[OK]" : "[MISMATCH]");

    /* Quick LBN map of the reserved file headers (1..5). */
    fprintf(fp, "    Reserved file headers    :\n");
    fprintf(fp, "      INDEXF.SYS  (1,1,0)   header LBN %u\n",
            h->iblb + h->ibsz + 0);
    fprintf(fp, "      BITMAP.SYS  (2,2,0)   header LBN %u\n",
            h->iblb + h->ibsz + 1);
    fprintf(fp, "      BADBLK.SYS  (3,3,0)   header LBN %u\n",
            h->iblb + h->ibsz + 2);
    fprintf(fp, "      000000.DIR  (4,4,0)   header LBN %u\n",
            h->iblb + h->ibsz + 3);
    fprintf(fp, "      CORIMG.SYS  (5,5,0)   header LBN %u\n",
            h->iblb + h->ibsz + 4);
}
