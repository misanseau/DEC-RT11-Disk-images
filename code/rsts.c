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
 * rsts.c - DEC RSTS/E disk pack decoder.
 *
 * Stage 4 layout (per Mayfield "RSTS/E Monitor Internals" ch.1):
 *
 *   In RDS 0.0, the MFD doubles as the UFD for [1,1].  That means a
 *   directory walk through the MFD encounters a MIX of MFD Name Entries
 *   (accounts, with US.UFD bit set in USTAT) and UFD Name Entries
 *   (files belonging to [1,1], with US.UFD bit clear).  We disambiguate
 *   on the USTAT byte at offset +8.
 *
 *   Directory link word format (Mayfield 1.2.12):
 *      bits 15-12  Block within directory cluster (0..UCLUS-1)
 *      bits 11-9   Cluster within cluster map (0..6)
 *      bits  8-4   Entry within block (blockette 0..31)
 *      bits  3-0   Flags  (bit 0 = in-use, others informational)
 *
 *   For a single-block walk (block=0, cluster=0), byte offset within
 *   the current block = (link & 0x01F0).
 *
 *   MFD Name Entry (US.UFD set):
 *      +0  ULNK    link to next entry
 *      +2  UNAM    PPN [project,programmer]  proj=high byte, prog=low
 *      +4  PASS1   account password, RAD-50 word 1 (3 chars)
 *      +6  PASS2   account password, RAD-50 word 2 (3 chars)
 *      +8  USTAT
 *      +9  unused (0x60 reserved)
 *      +10 UACNT
 *      +12 UAA     directory link to MFD Accounting Entry
 *      +14 UAR     DCN of first UFD cluster (0 if not allocated)
 *
 *   UFD Name Entry (US.UFD clear, applies inside UFDs and to mixed
 *   entries in [1,1]'s UFD):
 *      +0  ULNK    link to next entry
 *      +2  FNAME1  filename, RAD-50 word 1 (3 chars)
 *      +4  FNAME2  filename, RAD-50 word 2 (3 chars)  -> 6-char filename
 *      +6  FEXT    extension, RAD-50 (3 chars)
 *      +8  USTAT
 *      +9  UPROT
 *      +10 UACNT
 *      +12 UAA     directory link to UFD Accounting Entry
 *      +14 UAR     directory link to first Retrieval Entry (0 if size=0)
 *
 *   UFD Accounting Entry:
 *      +0  ULNK    link to Attributes Entry (optional)
 *      +2  UDLA    last access date    (RSTS internal date)
 *      +4  USIZ    file size in blocks (or LSB; with URTS1=0, +12 holds high)
 *      +6  UDC     creation date
 *      +8  UTC     creation time
 *      +10 URTS1   runtime system name word 1 (RAD-50)  / 0 = large file
 *      +12 URTS2   runtime system name word 2  / or size MSB if URTS1=0
 *      +14 UCLUS   file cluster size
 *
 *   USTAT bits (Mayfield 1.2.2.1 / 1.2.6.1):
 *      0x01  US.OUT  obsolete
 *      0x02  US.PLC  placed
 *      0x04  US.WRT  opened for write
 *      0x08  US.UPD  opened for update
 *      0x10  US.NOX  contiguous (always 1 for MFD entries)
 *      0x20  US.NOK  protected (always 1 for MFD entries)
 *      0x40  US.UFD  THIS IS AN MFD NAME ENTRY (ACCOUNT POINTER)
 *      0x80  US.DEL  marked for deletion
 *
 *   RSTS internal date  = (year - 1970) * 1000 + day_of_year
 *   RSTS internal time  = low 11 bits = minutes UNTIL midnight
 */
#include "rsts.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>   /* calloc, free */
#include <string.h>

/* Forward declarations (functions used before their definition). */
static int find_account_uar(FILE *fp, uint32_t mfd_lbn, uint32_t dcs,
                            uint16_t want_ppn, uint16_t *out_uar);

/* ----------------------------------------------------------------------
 *  RAD-50
 * ---------------------------------------------------------------------- */

static const char rad50_alpha[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789";

static void rad50_decode_word(uint16_t w, char out[4]) {
    out[0] = rad50_alpha[(w / 1600u) % 40u];
    out[1] = rad50_alpha[(w /   40u) % 40u];
    out[2] = rad50_alpha[ w           % 40u];
    out[3] = '\0';
}

static uint16_t le16(const uint8_t *buf, size_t off) {
    return (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
}

uint32_t rsts_dcs_for_size(uint32_t blocks) {
    uint32_t dcs = 1;
    while (dcs <= 32 && (blocks / dcs) >= 65536u) dcs <<= 1;
    return dcs;
}

/* ----------------------------------------------------------------------
 *  Pack label parser  (unchanged from earlier stages)
 * ---------------------------------------------------------------------- */

int rsts_parse_pack_label(const uint8_t *buf, uint32_t total_blocks,
                          rsts_pack_t *out) {
    char w1[4], w2[4]; int i;
    if (!buf || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->link        = le16(buf, RSTS_PL_LINK_OFF);
    out->used_flag   = le16(buf, RSTS_PL_USED_OFF);
    out->reserved    = le16(buf, RSTS_PL_RSVD_OFF);
    out->rds_level   = le16(buf, RSTS_PL_RDS_OFF);
    out->pcs         = le16(buf, RSTS_PL_PCS_OFF);
    out->pack_status = le16(buf, RSTS_PL_PSTAT_OFF);
    rad50_decode_word(le16(buf, RSTS_PL_ID1_OFF), w1);
    rad50_decode_word(le16(buf, RSTS_PL_ID2_OFF), w2);
    snprintf(out->pack_id, sizeof(out->pack_id), "%s%s", w1, w2);
    out->total_blocks = total_blocks;
    out->dcs          = rsts_dcs_for_size(total_blocks);
    out->mfd_lbn      = RSTS_MFD_DCN * out->dcs;
    out->used_ok = (out->used_flag == 0xFFFFu);
    out->pcs_ok  = (out->pcs >= 1 && out->pcs <= 64 &&
                    (out->pcs & (out->pcs - 1)) == 0);
    out->rds_ok  = (out->rds_level == 0 ||
                    ((out->rds_level >> 8) & 0xff) == 1);
    out->id_ok = 1;
    for (i = 0; i < 6; i++) {
        char c = out->pack_id[i];
        if (c == ' ') continue;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) continue;
        out->id_ok = 0; break;
    }
    if (strncmp(out->pack_id, "      ", 6) == 0) out->id_ok = 0;
    return out->used_ok && out->pcs_ok && out->rds_ok && out->id_ok;
}

int rsts_test(FILE *fp, uint32_t total_blocks) {
    uint8_t buf[RSTS_BLOCK_SIZE]; rsts_pack_t pk;
    uint32_t dcs = rsts_dcs_for_size(total_blocks);
    long long off = (long long)RSTS_MFD_DCN * dcs * RSTS_BLOCK_SIZE;
    if (!fp) return 0;
    if (RT_FSEEK64(fp, off, SEEK_SET) != 0) return 0;
    if (fread(buf, 1, sizeof(buf), fp) != sizeof(buf)) return 0;
    return rsts_parse_pack_label(buf, total_blocks, &pk);
}

/* ----------------------------------------------------------------------
 *  USTAT bits -> short symbolic string
 * ---------------------------------------------------------------------- */

static void ustat_string(uint8_t s, char *out, size_t n) {
    char buf[32]; int i = 0;
    if (s & 0x40) { if (i) buf[i++]='+'; buf[i++]='U'; buf[i++]='F'; buf[i++]='D'; }
    if (s & 0x20) { if (i) buf[i++]='+'; buf[i++]='N'; buf[i++]='K'; }
    if (s & 0x10) { if (i) buf[i++]='+'; buf[i++]='N'; buf[i++]='X'; }
    if (s & 0x08) { if (i) buf[i++]='+'; buf[i++]='U'; buf[i++]='P'; }
    if (s & 0x04) { if (i) buf[i++]='+'; buf[i++]='W'; buf[i++]='R'; }
    if (s & 0x02) { if (i) buf[i++]='+'; buf[i++]='P'; buf[i++]='L'; }
    if (s & 0x80) { if (i) buf[i++]='+'; buf[i++]='D'; buf[i++]='E'; buf[i++]='L'; }
    if (i == 0) buf[i++] = '-';
    buf[i] = '\0';
    snprintf(out, n, "%s", buf);
}

/* ----------------------------------------------------------------------
 *  RSTS date / time decoders
 * ---------------------------------------------------------------------- */

static void rsts_format_date(uint16_t d, char *out, size_t n) {
    static const char *mname[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    static const int mdays[2][12] = {
        {31,28,31,30,31,30,31,31,30,31,30,31},
        {31,29,31,30,31,30,31,31,30,31,30,31}
    };
    int year, doy, leap, mo, dy;
    if (d == 0) { snprintf(out, n, "          "); return; }
    year = 1970 + (d / 1000);
    doy  =        (d % 1000);
    leap = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 1 : 0;
    mo = 0; dy = doy;
    while (mo < 12 && dy > mdays[leap][mo]) {
        dy -= mdays[leap][mo];
        mo++;
    }
    if (mo >= 12) { snprintf(out, n, "%5u/???", (unsigned)d); return; }
    snprintf(out, n, "%2d-%s-%04d", dy, mname[mo], year);
}

static void rsts_format_time(uint16_t t, char *out, size_t n) {
    unsigned mins, h, m;
    if (t == 0) { snprintf(out, n, "        "); return; }
    mins = (unsigned)(t & 0x07FFu);
    if (mins == 0) { snprintf(out, n, "00:00"); return; }
    if (mins > 24u * 60u) { snprintf(out, n, "??:??"); return; }
    mins = (24u * 60u) - mins;
    h = mins / 60u;
    m = mins % 60u;
    snprintf(out, n, "%02u:%02u", h, m);
}

/* ----------------------------------------------------------------------
 *  Block read helper
 * ---------------------------------------------------------------------- */

static int read_block(FILE *fp, uint32_t lbn, uint8_t *buf) {
    if (RT_FSEEK64(fp, (long long)lbn * RSTS_BLOCK_SIZE, SEEK_SET) != 0)
        return -1;
    if (fread(buf, 1, RSTS_BLOCK_SIZE, fp) != RSTS_BLOCK_SIZE)
        return -1;
    return 0;
}

/* Mayfield 1.2.12 link word fields. */
static uint16_t link_byte_offset(uint16_t link) { return (uint16_t)(link & 0x01F0u); }
static uint16_t link_block(uint16_t link)       { return (uint16_t)((link >> 12) & 0x0Fu); }
static uint16_t link_cluster(uint16_t link)     { return (uint16_t)((link >>  9) & 0x07u); }

/* Resolve a directory link to a (block_lbn, byte_offset) pair, reading
 * the cluster map from the *current* block.  Returns 0 on success,
 * -1 if the link points to an unallocated cluster slot. */
static int resolve_link(const uint8_t *cur_blk, uint32_t dcs, uint16_t link,
                        uint32_t *out_lbn, size_t *out_off) {
    uint16_t blk_n = link_block(link);
    uint16_t clu_n = link_cluster(link);
    uint16_t cluster_dcn = le16(cur_blk, 0x1F0 + 2 + 2 * clu_n);
    if (cluster_dcn == 0) return -1;
    *out_lbn = (uint32_t)cluster_dcn * dcs + (uint32_t)blk_n;
    *out_off = link_byte_offset(link);
    return 0;
}

/* ----------------------------------------------------------------------
 *  Pretty-print one UFD Name Entry (file)
 * ---------------------------------------------------------------------- */

static void print_file_entry(FILE *fp, uint32_t dcs,
                             const uint8_t *blk, size_t p,
                             const char *prefix) {
    char     n1[4], n2[4], n3[4], stat_str[24];
    char     date_str[16], time_str[16];
    char     rts1[4], rts2[4];
    uint16_t fn1   = le16(blk, p + 2);
    uint16_t fn2   = le16(blk, p + 4);
    uint16_t fext  = le16(blk, p + 6);
    uint8_t  ustat = blk[p + 8];
    uint8_t  uprot = blk[p + 9];
    uint16_t acc   = le16(blk, p + 12);
    uint16_t retr  = le16(blk, p + 14);
    uint32_t size  = 0;
    uint16_t udc = 0, utc = 0, urts1 = 0, urts2 = 0, uclus = 0;

    rad50_decode_word(fn1,  n1);
    rad50_decode_word(fn2,  n2);
    rad50_decode_word(fext, n3);
    ustat_string(ustat, stat_str, sizeof(stat_str));

    if (acc != 0 && fp != NULL) {
        uint32_t alb;
        size_t   ao;
        uint8_t  local[RSTS_BLOCK_SIZE];
        if (resolve_link(blk, dcs, acc, &alb, &ao) == 0 &&
            ao + 16 <= RSTS_BLOCK_SIZE &&
            read_block(fp, alb, local) == 0) {
            uint16_t usiz = le16(local, ao + 4);
            udc   = le16(local, ao + 6);
            utc   = le16(local, ao + 8);
            urts1 = le16(local, ao + 10);
            urts2 = le16(local, ao + 12);
            uclus = le16(local, ao + 14);
            if (urts1 == 0) size = ((uint32_t)urts2 << 16) | (uint32_t)usiz;
            else            size = (uint32_t)usiz;
        }
    }
    rad50_decode_word(urts1, rts1);
    rad50_decode_word(urts2, rts2);
    rsts_format_date(udc, date_str, sizeof(date_str));
    rsts_format_time(utc, time_str, sizeof(time_str));

    printf("%s%s%s.%s  %7u blk  %s %s  RTS=%s%s  prot=%03o  stat=%s  "
           "clus=%u  retr=0x%04x\n",
           prefix, n1, n2, n3,
           (unsigned)size,
           date_str, time_str,
           (urts1 || urts2) ? rts1 : "   ",
           (urts1 || urts2) ? rts2 : "   ",
           (unsigned)uprot, stat_str,
           (unsigned)uclus, (unsigned)retr);
}

/* ----------------------------------------------------------------------
 *  Walk a directory (MFD or UFD), distinguishing accounts vs files.
 *
 *   mode == 0 :  walk a UFD - all entries are files.
 *   mode == 1 :  walk the MFD - entries with US.UFD set are accounts
 *                (printed with PPN + password + UFD pointer); entries
 *                with US.UFD clear are files in the [1,1] UFD that
 *                shares the MFD blocks.  Accounts get queued for a
 *                later UFD walk by the caller.
 *
 *   For each account discovered, on_account(arg, ppn, uar_dcn) is called.
 *
 *   Returns total entries walked (or -1 on read error).
 * ---------------------------------------------------------------------- */

typedef void (*account_cb)(void *arg, uint16_t ppn, uint16_t uar_dcn,
                           const char *password);

static int walk_directory(FILE *fp, uint32_t first_lbn, uint32_t dcs,
                          int mode, const char *file_indent,
                          account_cb cb, void *cb_arg,
                          int *out_files, long long *out_blocks) {
    uint8_t  blk[RSTS_BLOCK_SIZE];
    uint32_t cur_lbn = first_lbn;
    uint16_t cur, prev_cur;
    int      guard;
    int      total = 0;

    if (read_block(fp, cur_lbn, blk) != 0) {
        fprintf(stderr, "?Cannot read directory at LBN=%u\n",
                (unsigned)cur_lbn);
        return -1;
    }

    cur = le16(blk, 0);     /* link to first entry from the label */
    prev_cur = 0;

    for (guard = 0; cur != 0 && guard < 4096; guard++) {
        uint32_t target_lbn;
        size_t   p;
        uint16_t next_link;
        uint8_t  ustat;

        if (resolve_link(blk, dcs, cur, &target_lbn, &p) != 0) {
            fprintf(stderr,
                    "  ?Link 0x%04x -> unallocated cluster; chain broken\n",
                    cur);
            break;
        }
        if (p + 16 > RSTS_BLOCK_SIZE) {
            fprintf(stderr, "  ?Link 0x%04x byte offset out of range\n", cur);
            break;
        }
        if (target_lbn != cur_lbn) {
            if (read_block(fp, target_lbn, blk) != 0) {
                fprintf(stderr, "  ?Cannot read dir block LBN=%u (link=0x%04x)\n",
                        (unsigned)target_lbn, cur);
                break;
            }
            cur_lbn = target_lbn;
        }

        next_link = le16(blk, p + 0);
        ustat     = blk[p + 8];

        if (mode == 1 && (ustat & 0x40u)) {
            char     pw1[4], pw2[4], stat_str[24], pwbuf[8];
            uint16_t ppn   = le16(blk, p + 2);
            uint16_t pass1 = le16(blk, p + 4);
            uint16_t pass2 = le16(blk, p + 6);
            uint16_t uar   = le16(blk, p + 14);

            rad50_decode_word(pass1, pw1);
            rad50_decode_word(pass2, pw2);
            ustat_string(ustat, stat_str, sizeof(stat_str));
            snprintf(pwbuf, sizeof(pwbuf), "%s%s", pw1, pw2);
            printf("\nACCT  [%3u,%3u]  pass=%-7s  stat=%s  UAR=%u",
                   (unsigned)((ppn >> 8) & 0xff),
                   (unsigned)( ppn       & 0xff),
                   pwbuf, stat_str, (unsigned)uar);
            if (uar != 0) {
                printf("  -> UFD@LBN=%u\n", (unsigned)uar * (unsigned)dcs);
                if (cb) cb(cb_arg, ppn, uar, pwbuf);
            } else {
                printf("  (no UFD allocated)\n");
            }
        } else {
            print_file_entry(fp, dcs, blk, p, file_indent);
            if (out_files)  (*out_files)++;
            if (out_blocks) {
                uint16_t acc = le16(blk, p + 12);
                uint32_t alb;
                size_t   ao;
                uint8_t  local[RSTS_BLOCK_SIZE];
                if (acc != 0 &&
                    resolve_link(blk, dcs, acc, &alb, &ao) == 0 &&
                    ao + 16 <= RSTS_BLOCK_SIZE &&
                    read_block(fp, alb, local) == 0) {
                    uint16_t usiz  = le16(local, ao + 4);
                    uint16_t urts1 = le16(local, ao + 10);
                    uint16_t urts2 = le16(local, ao + 12);
                    uint32_t size  = (urts1 == 0)
                        ? (((uint32_t)urts2 << 16) | usiz)
                        : (uint32_t)usiz;
                    (*out_blocks) += (long long)size;
                }
            }
        }
        total++;

        if (next_link == prev_cur) break;
        prev_cur = cur;
        cur      = next_link;
    }
    if (guard >= 4096) {
        fprintf(stderr, "  ?Directory chain >4096 entries; truncated\n");
    }
    return total;
}

/* ----------------------------------------------------------------------
 *  DIR top-level
 * ---------------------------------------------------------------------- */

/* Small queue to defer UFD walks until after the MFD walk completes,
 * so the listing reads top-down nicely. */
#define MAX_QUEUED_ACCOUNTS 32
typedef struct {
    uint16_t ppn;
    uint16_t uar_dcn;
    char     password[8];
} acct_entry_t;

typedef struct {
    acct_entry_t list[MAX_QUEUED_ACCOUNTS];
    int          n;
} acct_queue_t;

static void enqueue_account(void *arg, uint16_t ppn, uint16_t uar_dcn,
                            const char *password) {
    acct_queue_t *q = (acct_queue_t *)arg;
    if (q->n < MAX_QUEUED_ACCOUNTS) {
        q->list[q->n].ppn     = ppn;
        q->list[q->n].uar_dcn = uar_dcn;
        snprintf(q->list[q->n].password, sizeof(q->list[q->n].password),
                 "%s", password);
        q->n++;
    }
}

/* Parse "[g,p]" into integers.  Returns 1 on success. */
static int parse_uic_arg_local(const char *arg, int *gout, int *pout) {
    int gv = 0, pv = 0;
    const char *q;
    if (!arg || arg[0] != '[') return 0;
    q = arg + 1;
    while (*q >= '0' && *q <= '9') { gv = gv*10 + (*q - '0'); q++; }
    if (*q != ',' || gv > 255) return 0;
    q++;
    while (*q >= '0' && *q <= '9') { pv = pv*10 + (*q - '0'); q++; }
    if (*q != ']' || pv > 255) return 0;
    *gout = gv; *pout = pv;
    return 1;
}

int rsts_cmd_dir(Mount *m, const char *uic_arg) {
    uint8_t       buf[RSTS_BLOCK_SIZE];
    rsts_pack_t   pk;
    long long     off;
    long long     grand_blocks = 0;
    int           grand_files  = 0;
    int           i;
    acct_queue_t  q;

    if (!m || !m->fp) return -1;

    off = (long long)RSTS_MFD_DCN * rsts_dcs_for_size(m->total_blocks)
          * RSTS_BLOCK_SIZE;
    if (RT_FSEEK64(m->fp, off, SEEK_SET) != 0 ||
        fread(buf, 1, sizeof(buf), m->fp) != sizeof(buf)) {
        fprintf(stderr, "?Cannot read MFD block of '%s'\n", m->path);
        return -1;
    }
    if (!rsts_parse_pack_label(buf, m->total_blocks, &pk)) {
        fprintf(stderr, "?'%s' does not look like a RSTS/E pack\n", m->path);
        return -1;
    }

    /* Single-UFD mode: list just the one account and its files. */
    if (uic_arg && uic_arg[0]) {
        int g, p;
        uint16_t ppn, uar_dcn = 0;
        uint32_t ufd_lbn;
        int      files = 0;
        long long blocks = 0;
        if (!parse_uic_arg_local(uic_arg, &g, &p)) {
            fprintf(stderr, "?Bad UIC '%s' (expected [g,p])\n", uic_arg);
            return -1;
        }
        ppn = (uint16_t)(((g & 0xff) << 8) | (p & 0xff));
        if (ppn == 0x0101) {
            ufd_lbn = pk.mfd_lbn;
        } else {
            if (find_account_uar(m->fp, pk.mfd_lbn, pk.dcs, ppn,
                                 &uar_dcn) != 0) {
                fprintf(stderr, "?Account [%d,%d] not found in MFD\n",
                        g, p);
                return -1;
            }
            if (uar_dcn == 0) {
                fprintf(stderr, "?Account [%d,%d] has no UFD allocated\n",
                        g, p);
                return -1;
            }
            ufd_lbn = (uint32_t)uar_dcn * pk.dcs;
        }
        printf(" UFD [%d,%d] of pack %s  @LBN=%u\n",
               g, p, pk.pack_id, (unsigned)ufd_lbn);
        printf("  Filename       Size  Date         Time   RTS\n");
        printf("  ----------- ------- -----------  -----  ------\n");
        walk_directory(m->fp, ufd_lbn, pk.dcs, 0, "  ",
                       NULL, NULL, &files, &blocks);
        printf("\n  %d files, %lld blocks\n", files, blocks);
        return 0;
    }

    /* Full pack walk: pack header, MFD, then descend each UFD. */
    printf(" Pack: %s  (RSTS/E", pk.pack_id);
    if (pk.rds_level == 0) {
        printf(" RDS 0.0");
    } else {
        printf(" RDS %u.%u",
               (unsigned)((pk.rds_level >> 8) & 0xff),
               (unsigned)( pk.rds_level       & 0xff));
    }
    printf(")\n");
    printf("  PCS=%u  DCS=%u  MFD LBN=%u  Pack status=0x%04x\n",
           (unsigned)pk.pcs, (unsigned)pk.dcs,
           (unsigned)pk.mfd_lbn, (unsigned)pk.pack_status);

    q.n = 0;
    printf("\n[ MFD walk -- accounts and intermixed [1,1] files ]\n");
    walk_directory(m->fp, pk.mfd_lbn, pk.dcs, 1, "  FILE  ",
                   enqueue_account, &q, &grand_files, &grand_blocks);

    for (i = 0; i < q.n; i++) {
        uint32_t ufd_lbn;
        if (q.list[i].uar_dcn == 0) continue;
        if (q.list[i].ppn == 0x0101) continue;
        ufd_lbn = (uint32_t)q.list[i].uar_dcn * pk.dcs;
        printf("\n[ UFD [%u,%u] @LBN=%u ]\n",
               (unsigned)((q.list[i].ppn >> 8) & 0xff),
               (unsigned)( q.list[i].ppn       & 0xff),
               (unsigned)ufd_lbn);
        walk_directory(m->fp, ufd_lbn, pk.dcs, 0, "  ",
                       NULL, NULL, &grand_files, &grand_blocks);
    }
    if (q.n >= MAX_QUEUED_ACCOUNTS) {
        fprintf(stderr,
                "?More than %d accounts -- listing truncated\n",
                MAX_QUEUED_ACCOUNTS);
    }

    printf("\n  Summary: %d files in %d accounts, %lld total blocks\n",
           grand_files, q.n, grand_blocks);
    return 0;
}

/* ----------------------------------------------------------------------
 *  COPY OUT: lookup a file by PPN + name.ext, walk Retrieval Entries,
 *  write blocks to a host file.
 * ---------------------------------------------------------------------- */

/* Encode up to 3 ASCII chars (case-folded, padded with spaces) into a
 * single RAD-50 word.  Non-RAD-50 characters become space (=0). */
static uint16_t rad50_encode_word(const char *src, int max) {
    int  i;
    char chars[3] = {' ', ' ', ' '};
    uint16_t w = 0;
    for (i = 0; i < 3 && i < max && src[i]; i++) chars[i] = src[i];
    for (i = 0; i < 3; i++) {
        char c = chars[i];
        int idx = 0;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == ' ')                                 idx = 0;
        else if (c >= 'A' && c <= 'Z')                idx = 1 + (c - 'A');
        else if (c == '$')                            idx = 27;
        else if (c == '.')                            idx = 28;
        else if (c == '%')                            idx = 29;
        else if (c >= '0' && c <= '9')                idx = 30 + (c - '0');
        else                                          idx = 0;
        w = (uint16_t)(w * 40u + (unsigned)idx);
    }
    return w;
}

/* Encode a "NAME.EXT" pair (or just "NAME") into 4 RAD-50 words:
 *   words[0..1] = filename (up to 6 chars, space-padded)
 *   words[2]    = (unused, kept 0)  -- our entries are 6+3 not 9
 *   words[3]    = extension (up to 3 chars)
 *
 * Returns 0 on success.  We accept name lengths 1..6 and ext 0..3. */
static void rad50_encode_filename(const char *name, const char *type,
                                  uint16_t out[3]) {
    int nl = (int)strlen(name);
    char pad[7] = {' ',' ',' ',' ',' ',' ','\0'};
    int  i;
    if (nl > 6) nl = 6;
    for (i = 0; i < nl; i++) pad[i] = name[i];
    out[0] = rad50_encode_word(pad,     3);
    out[1] = rad50_encode_word(pad + 3, 3);
    out[2] = rad50_encode_word(type ? type : "", 3);
}

/* Walk the MFD to find the UAR (DCN of UFD) for a given PPN.  Returns
 * 0 + sets *out_uar on success; -1 if the PPN isn't an account. */
static int find_account_uar(FILE *fp, uint32_t mfd_lbn, uint32_t dcs,
                            uint16_t want_ppn, uint16_t *out_uar) {
    uint8_t  blk[RSTS_BLOCK_SIZE];
    uint32_t cur_lbn = mfd_lbn;
    uint16_t cur, prev_cur = 0;
    int      guard;

    if (read_block(fp, cur_lbn, blk) != 0) return -1;
    cur = le16(blk, 0);
    for (guard = 0; cur != 0 && guard < 4096; guard++) {
        uint32_t tlbn;
        size_t   p;
        uint16_t next_link;
        uint8_t  ustat;

        if (resolve_link(blk, dcs, cur, &tlbn, &p) != 0) return -1;
        if (p + 16 > RSTS_BLOCK_SIZE) return -1;
        if (tlbn != cur_lbn) {
            if (read_block(fp, tlbn, blk) != 0) return -1;
            cur_lbn = tlbn;
        }
        next_link = le16(blk, p + 0);
        ustat     = blk[p + 8];
        if ((ustat & 0x40u) && le16(blk, p + 2) == want_ppn) {
            *out_uar = le16(blk, p + 14);
            return 0;
        }
        if (next_link == prev_cur) break;
        prev_cur = cur;
        cur      = next_link;
    }
    return -1;
}

/* Walk a UFD looking for a file whose 3 RAD-50 name words match
 * `want_words[0..2]` (FNAME word 1, FNAME word 2, EXT).  On success
 * fills *out_size_blocks, *out_retr_link, *out_fcs and returns 0.
 * Returns -1 if not found. */
static int find_file_in_ufd(FILE *fp, uint32_t ufd_lbn, uint32_t dcs,
                            const uint16_t want_words[3],
                            uint32_t *out_size, uint16_t *out_retr_link,
                            uint16_t *out_fcs) {
    uint8_t  blk[RSTS_BLOCK_SIZE];
    uint32_t cur_lbn = ufd_lbn;
    uint16_t cur, prev_cur = 0;
    int      guard;

    if (read_block(fp, cur_lbn, blk) != 0) return -1;
    cur = le16(blk, 0);
    for (guard = 0; cur != 0 && guard < 4096; guard++) {
        uint32_t tlbn;
        size_t   p;
        uint16_t next_link, fn1, fn2, fext, acc;
        uint8_t  ustat;

        if (resolve_link(blk, dcs, cur, &tlbn, &p) != 0) return -1;
        if (p + 16 > RSTS_BLOCK_SIZE) return -1;
        if (tlbn != cur_lbn) {
            if (read_block(fp, tlbn, blk) != 0) return -1;
            cur_lbn = tlbn;
        }
        next_link = le16(blk, p + 0);
        ustat     = blk[p + 8];

        /* Skip MFD-style entries (US.UFD set) - those are accounts,
         * not files.  In a true UFD this should never happen, but in
         * RDS 0.0 [1,1]'s UFD == MFD so we'll see them. */
        if (!(ustat & 0x40u)) {
            fn1  = le16(blk, p + 2);
            fn2  = le16(blk, p + 4);
            fext = le16(blk, p + 6);
            if (fn1 == want_words[0] && fn2 == want_words[1] &&
                fext == want_words[2]) {
                acc = le16(blk, p + 12);
                *out_retr_link = le16(blk, p + 14);
                *out_size      = 0;
                *out_fcs       = 0;
                if (acc != 0) {
                    uint32_t alb;
                    size_t   ao;
                    uint8_t  abuf[RSTS_BLOCK_SIZE];
                    if (resolve_link(blk, dcs, acc, &alb, &ao) == 0 &&
                        ao + 16 <= RSTS_BLOCK_SIZE &&
                        read_block(fp, alb, abuf) == 0) {
                        uint16_t usiz  = le16(abuf, ao + 4);
                        uint16_t urts1 = le16(abuf, ao + 10);
                        uint16_t urts2 = le16(abuf, ao + 12);
                        *out_fcs       = le16(abuf, ao + 14);
                        *out_size      = (urts1 == 0)
                            ? (((uint32_t)urts2 << 16) | usiz)
                            : (uint32_t)usiz;
                    }
                    /* Caller's blk[] is still valid (we used abuf[]). */
                }
                return 0;
            }
        }
        if (next_link == prev_cur) break;
        prev_cur = cur;
        cur      = next_link;
    }
    return -1;
}

/* Walk the Retrieval Entry chain starting at `retr_link` (interpreted
 * relative to the UFD at ufd_lbn), and emit each cluster's blocks to
 * the output FILE.  Stops after `max_blocks` blocks have been written
 * (the file's true size from the accounting entry).  Returns the
 * number of blocks written, or -1 on error. */
static long extract_via_retrieval(FILE *fp, uint32_t ufd_lbn, uint32_t dcs,
                                  uint16_t retr_link, uint16_t fcs,
                                  uint32_t max_blocks, FILE *out) {
    uint8_t  blk[RSTS_BLOCK_SIZE];
    uint32_t cur_lbn = ufd_lbn;
    uint16_t cur = retr_link, prev_cur = 0;
    int      guard;
    long     written = 0;
    uint8_t  data[RSTS_BLOCK_SIZE];

    if (fcs == 0) fcs = 1;

    if (read_block(fp, cur_lbn, blk) != 0) return -1;

    for (guard = 0; cur != 0 && guard < 4096; guard++) {
        uint32_t tlbn;
        size_t   p;
        uint16_t next_link;
        int      i;

        if (resolve_link(blk, dcs, cur, &tlbn, &p) != 0) return -1;
        if (p + 16 > RSTS_BLOCK_SIZE) return -1;
        if (tlbn != cur_lbn) {
            if (read_block(fp, tlbn, blk) != 0) return -1;
            cur_lbn = tlbn;
        }
        next_link = le16(blk, p + 0);

        /* The 7 cluster DCNs follow at offsets 2,4,6,8,10,12,14. */
        for (i = 0; i < 7; i++) {
            uint16_t dcn = le16(blk, p + 2 + 2 * i);
            uint32_t k;
            if (dcn == 0) continue;
            for (k = 0; k < fcs; k++) {
                uint32_t lbn = (uint32_t)dcn * dcs + k;
                if ((uint32_t)written >= max_blocks) return written;
                if (read_block(fp, lbn, data) != 0) {
                    fprintf(stderr, "?Read error at LBN=%u\n", (unsigned)lbn);
                    return -1;
                }
                if (fwrite(data, 1, RSTS_BLOCK_SIZE, out) != RSTS_BLOCK_SIZE) {
                    fprintf(stderr, "?Write error to host file\n");
                    return -1;
                }
                written++;
            }
        }
        if (next_link == prev_cur) break;
        prev_cur = cur;
        cur      = next_link;
    }
    return written;
}

int rsts_copy_out(Mount *m, int g, int p,
                  const char *name, const char *type,
                  const char *host_path) {
    uint8_t      pkbuf[RSTS_BLOCK_SIZE];
    rsts_pack_t  pk;
    uint16_t     ppn   = (uint16_t)(((g & 0xff) << 8) | (p & 0xff));
    uint16_t     uar_dcn = 0;
    uint32_t     ufd_lbn;
    uint16_t     want[3];
    uint32_t     fsize = 0;
    uint16_t     retr_link = 0, fcs = 0;
    long         written;
    FILE        *out;

    if (!m || !m->fp || !name || !host_path) return -1;

    /* Re-read the pack label to get the current DCS and validate. */
    if (RT_FSEEK64(m->fp, (long long)RSTS_MFD_DCN *
                   rsts_dcs_for_size(m->total_blocks) * RSTS_BLOCK_SIZE,
                   SEEK_SET) != 0 ||
        fread(pkbuf, 1, sizeof(pkbuf), m->fp) != sizeof(pkbuf) ||
        !rsts_parse_pack_label(pkbuf, m->total_blocks, &pk)) {
        fprintf(stderr, "?'%s' is not a RSTS/E pack\n", m->path);
        return -1;
    }

    /* Resolve PPN to UFD location.  [1,1] *is* the MFD. */
    if (ppn == 0x0101) {
        ufd_lbn = pk.mfd_lbn;
    } else {
        if (find_account_uar(m->fp, pk.mfd_lbn, pk.dcs, ppn, &uar_dcn) != 0) {
            fprintf(stderr, "?Account [%d,%d] not found in MFD\n", g, p);
            return -1;
        }
        if (uar_dcn == 0) {
            fprintf(stderr, "?Account [%d,%d] has no UFD allocated\n", g, p);
            return -1;
        }
        ufd_lbn = (uint32_t)uar_dcn * pk.dcs;
    }

    /* Encode requested name into RAD-50 words. */
    rad50_encode_filename(name, type, want);

    if (find_file_in_ufd(m->fp, ufd_lbn, pk.dcs, want,
                         &fsize, &retr_link, &fcs) != 0) {
        fprintf(stderr, "?File '%s.%s' not found in [%d,%d]\n",
                name, type ? type : "", g, p);
        return -1;
    }
    if (fsize == 0 || retr_link == 0) {
        fprintf(stderr, "?File '%s.%s' has no allocated blocks\n",
                name, type ? type : "");
        /* Still create an empty host file. */
        out = fopen(host_path, "wb");
        if (!out) {
            fprintf(stderr, "?Cannot create '%s'\n", host_path);
            return -1;
        }
        fclose(out);
        return 0;
    }

    out = fopen(host_path, "wb");
    if (!out) {
        fprintf(stderr, "?Cannot create '%s'\n", host_path);
        return -1;
    }
    written = extract_via_retrieval(m->fp, ufd_lbn, pk.dcs,
                                    retr_link, fcs, fsize, out);
    fclose(out);
    if (written < 0) {
        fprintf(stderr, "?Extraction of '%s.%s' failed\n",
                name, type ? type : "");
        return -1;
    }
    printf("Copied [%d,%d]%s.%s -> '%s' (%ld blocks, FCS=%u)\n",
           g, p, name, type ? type : "", host_path,
           written, (unsigned)fcs);
    return 0;
}

/* ----------------------------------------------------------------------
 *  Wildcard COPY -- glob across one UFD
 * ---------------------------------------------------------------------- */

/* Case-insensitive shell-style glob: '*' = zero+ chars, '?' = any one. */
static int rsts_glob_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            while (pat[1] == '*') pat++;
            if (pat[1] == '\0') return 1;
            pat++;
            while (*name) {
                if (rsts_glob_match(pat, name)) return 1;
                name++;
            }
            return rsts_glob_match(pat, name);
        } else if (*pat == '?') {
            if (!*name) return 0;
            pat++; name++;
        } else {
            char a = *pat, b = *name;
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) return 0;
            pat++; name++;
        }
    }
    return *name == '\0';
}

/* Trim trailing spaces in place. */
static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n-1] == ' ') { s[--n] = '\0'; }
}

/* Apply case mode in-place (0=UC, 1=LC, 2=NC). */
static void apply_case_local(char *s, int mode) {
    if (mode == 2) return;
    for (; *s; s++) {
        if (mode == 0 && *s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
        if (mode == 1 && *s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
    }
}

static int file_exists_local(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int rsts_copy_wild(Mount *m, int g, int p,
                   const char *name_pat, const char *type_pat,
                   const char *host_dir, int case_mode,
                   int *out_copied, int *out_skipped, int *out_errors) {
    uint8_t      pkbuf[RSTS_BLOCK_SIZE];
    rsts_pack_t  pk;
    uint16_t     ppn = (uint16_t)(((g & 0xff) << 8) | (p & 0xff));
    uint16_t     uar_dcn = 0;
    uint32_t     ufd_lbn;
    uint8_t      blk[RSTS_BLOCK_SIZE];
    uint32_t     cur_lbn;
    uint16_t     cur, prev_cur;
    int          guard;
    int          copied = 0, skipped = 0, errors = 0;
    int          dir_hint;
    size_t       hostlen;

    if (!m || !m->fp || !name_pat) return -1;

    if (RT_FSEEK64(m->fp, (long long)RSTS_MFD_DCN *
                   rsts_dcs_for_size(m->total_blocks) * RSTS_BLOCK_SIZE,
                   SEEK_SET) != 0 ||
        fread(pkbuf, 1, sizeof(pkbuf), m->fp) != sizeof(pkbuf) ||
        !rsts_parse_pack_label(pkbuf, m->total_blocks, &pk)) {
        fprintf(stderr, "?'%s' is not a RSTS/E pack\n", m->path);
        return -1;
    }

    if (ppn == 0x0101) {
        ufd_lbn = pk.mfd_lbn;
    } else {
        if (find_account_uar(m->fp, pk.mfd_lbn, pk.dcs, ppn, &uar_dcn) != 0) {
            fprintf(stderr, "?Account [%d,%d] not found in MFD\n", g, p);
            return -1;
        }
        if (uar_dcn == 0) {
            fprintf(stderr, "?Account [%d,%d] has no UFD allocated\n", g, p);
            return -1;
        }
        ufd_lbn = (uint32_t)uar_dcn * pk.dcs;
    }

    /* Check whether host_dir looks like a directory hint. */
    hostlen  = host_dir ? strlen(host_dir) : 0;
    dir_hint = (hostlen > 0 &&
                (host_dir[hostlen-1] == '/' || host_dir[hostlen-1] == '\\'));
    if (!dir_hint) {
        fprintf(stderr, "?Wildcard COPY destination must end in '/' or '\\\\'\n");
        return -1;
    }

    cur_lbn = ufd_lbn;
    if (read_block(m->fp, cur_lbn, blk) != 0) return -1;
    cur = le16(blk, 0);
    prev_cur = 0;

    for (guard = 0; cur != 0 && guard < 4096; guard++) {
        uint32_t tlbn;
        size_t   pp;
        uint16_t next_link, fn1, fn2, fext, acc, retr;
        uint8_t  ustat;
        char     n1[4], n2[4], n3[4];
        char     name_str[8], type_str[4], host_path[MOUNT_PATH_MAX];
        char     hostname[16];
        uint32_t fsize = 0;
        uint16_t fcs   = 0;

        if (resolve_link(blk, pk.dcs, cur, &tlbn, &pp) != 0) break;
        if (pp + 16 > RSTS_BLOCK_SIZE) break;
        if (tlbn != cur_lbn) {
            if (read_block(m->fp, tlbn, blk) != 0) break;
            cur_lbn = tlbn;
        }
        next_link = le16(blk, pp + 0);
        ustat     = blk[pp + 8];

        /* Skip account entries. */
        if (ustat & 0x40u) goto next;

        fn1  = le16(blk, pp + 2);
        fn2  = le16(blk, pp + 4);
        fext = le16(blk, pp + 6);
        acc  = le16(blk, pp + 12);
        retr = le16(blk, pp + 14);
        rad50_decode_word(fn1,  n1);
        rad50_decode_word(fn2,  n2);
        rad50_decode_word(fext, n3);
        snprintf(name_str, sizeof(name_str), "%s%s", n1, n2);
        snprintf(type_str, sizeof(type_str), "%s", n3);
        rstrip(name_str);
        rstrip(type_str);

        /* Glob match. */
        if (!rsts_glob_match(name_pat, name_str)) goto next;
        if (type_pat && *type_pat &&
            !rsts_glob_match(type_pat, type_str)) goto next;

        /* Build host path. */
        snprintf(hostname, sizeof(hostname), "%s%s%s",
                 name_str, type_str[0] ? "." : "", type_str);
        apply_case_local(hostname, case_mode);
        snprintf(host_path, sizeof(host_path), "%s%s", host_dir, hostname);

        if (file_exists_local(host_path)) {
            fprintf(stderr, "  %s -> %s  SKIPPED (host exists)\n",
                    name_str, host_path);
            skipped++;
            goto next;
        }

        /* Read accounting blockette. */
        if (acc != 0) {
            uint32_t alb;
            size_t   ao;
            uint8_t  abuf[RSTS_BLOCK_SIZE];
            if (resolve_link(blk, pk.dcs, acc, &alb, &ao) == 0 &&
                ao + 16 <= RSTS_BLOCK_SIZE &&
                read_block(m->fp, alb, abuf) == 0) {
                uint16_t usiz  = le16(abuf, ao + 4);
                uint16_t urts1 = le16(abuf, ao + 10);
                uint16_t urts2 = le16(abuf, ao + 12);
                fcs            = le16(abuf, ao + 14);
                fsize          = (urts1 == 0)
                    ? (((uint32_t)urts2 << 16) | usiz)
                    : (uint32_t)usiz;
            }
        }

        if (fsize == 0 || retr == 0) {
            FILE *out = fopen(host_path, "wb");
            if (out) { fclose(out); copied++;
                       printf("  %s -> %s  (empty)\n", name_str, host_path); }
            else     { fprintf(stderr, "  ?Cannot create '%s'\n", host_path);
                       errors++; }
        } else {
            FILE *out = fopen(host_path, "wb");
            long  written;
            if (!out) {
                fprintf(stderr, "  ?Cannot create '%s'\n", host_path);
                errors++;
                goto next;
            }
            written = extract_via_retrieval(m->fp, ufd_lbn, pk.dcs,
                                            retr, fcs, fsize, out);
            fclose(out);
            if (written < 0) {
                fprintf(stderr, "  ?Extract failed for '%s'\n", name_str);
                errors++;
            } else {
                printf("  %s.%s -> %s  (%ld blocks)\n",
                       name_str, type_str, host_path, written);
                copied++;
            }
            /* Reload our walk-position block since extract did random I/O. */
            if (read_block(m->fp, cur_lbn, blk) != 0) break;
        }

    next:
        if (next_link == prev_cur) break;
        prev_cur = cur;
        cur      = next_link;
    }

    if (out_copied)  *out_copied  = copied;
    if (out_skipped) *out_skipped = skipped;
    if (out_errors)  *out_errors  = errors;
    printf("COPY summary: %d copied, %d skipped, %d errors\n",
           copied, skipped, errors);
    return errors == 0 ? 0 : -1;
}

/* ======================================================================
 *  RSTS/E COPY IN: append a host file as a new entry in UFD [g,p].
 *
 *  Plan:
 *    1.  Locate UFD via [g,p].  Find SATT.SYS to know what's free.
 *    2.  Encode filename/ext.  Compute file_clusters = ceil(blocks/PCS)
 *        (we use FCS = PCS for simplicity, single Retrieval Entry).
 *    3.  Find a contiguous run of `file_clusters` free PCN bits in SATT.
 *    4.  Find 3 free blockettes in the UFD (Name, Accounting, Retrieval).
 *    5.  Find tail of Name Entry chain (entry whose ULNK == 0).
 *    6.  Build the 3 blockettes with proper links.
 *    7.  In dry-run: print everything, exit.  Else:
 *          - write SATT.SYS (set bits)
 *          - write the 3 UFD blockettes
 *          - patch the previous tail ULNK to point at the new Name Entry
 *          - stream host data to LBN = DCN * DCS for each cluster
 * ====================================================================== */

#include <sys/stat.h>
#include <time.h>

static void le16_write(uint8_t *buf, size_t off, uint16_t v) {
    buf[off]     = (uint8_t)(v & 0xff);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xff);
}

/* Encode a directory link from (block_in_cluster, cluster_idx,
 * blockette_in_block, flags). */
static uint16_t make_link(int block, int cluster, int blkt, int flags) {
    return (uint16_t)(((block & 0xf) << 12) |
                      ((cluster & 0x7) << 9) |
                      ((blkt & 0x1f) << 4) |
                      (flags & 0xf));
}

/* Compute today's RSTS internal date and time. */
static uint16_t today_rsts_date(void) {
    int y, mo, d;
    int doy;
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leap;
    int i;
    today_ymd(&y, &mo, &d);
    leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 1 : 0;
    doy = d;
    for (i = 0; i < mo - 1; i++) {
        doy += mdays[i] + ((i == 1) ? leap : 0);
    }
    /* date = (year - 1970) * 1000 + day_of_year */
    return (uint16_t)(((y - 1970) * 1000 + doy) & 0xffff);
}

static uint16_t now_rsts_time(void) {
    time_t now = time(NULL);
    struct tm lt;
    int       mins;
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    struct tm *p = localtime(&now);
    lt = *p;
#endif
    mins = lt.tm_hour * 60 + lt.tm_min;
    /* Stored as minutes-until-midnight (low 11 bits). */
    if (mins == 0) return 0;
    return (uint16_t)((24 * 60 - mins) & 0x07ff);
}

/* Locate SATT.SYS by walking [0,1] UFD.  Returns 0 + sets *out_retr +
 * *out_size on success.  out_retr is the directory link of the first
 * Retrieval Entry; out_size is in blocks. */
static int find_satt_info(FILE *fp, uint32_t mfd_lbn, uint32_t dcs,
                          uint16_t *out_retr_link, uint32_t *out_size) {
    uint16_t  uar = 0;
    uint32_t  ufd_lbn;
    uint8_t   blk[RSTS_BLOCK_SIZE];
    uint16_t  cur, prev = 0;
    int       guard;
    uint32_t  cur_lbn;

    if (find_account_uar(fp, mfd_lbn, dcs, 0x0001, &uar) != 0 || uar == 0) {
        return -1;
    }
    ufd_lbn = (uint32_t)uar * dcs;
    if (read_block(fp, ufd_lbn, blk) != 0) return -1;
    cur_lbn = ufd_lbn;
    cur = le16(blk, 0);

    /* SATT in RAD-50: "SAT", "T  ", "SYS" */
    {
        uint16_t want_n1 = 0x7710u;   /* "SAT" */
        uint16_t want_n2 = 0x7d00u;   /* "T  " */
        uint16_t want_e  = 0x7abbu;   /* "SYS" */
        for (guard = 0; cur != 0 && guard < 4096; guard++) {
            uint32_t tlbn;
            size_t   p;
            uint16_t next_link;
            uint8_t  ustat;
            if (resolve_link(blk, dcs, cur, &tlbn, &p) != 0) return -1;
            if (p + 16 > RSTS_BLOCK_SIZE) return -1;
            if (tlbn != cur_lbn) {
                if (read_block(fp, tlbn, blk) != 0) return -1;
                cur_lbn = tlbn;
            }
            next_link = le16(blk, p + 0);
            ustat     = blk[p + 8];
            if (!(ustat & 0x40u)) {
                uint16_t fn1  = le16(blk, p + 2);
                uint16_t fn2  = le16(blk, p + 4);
                uint16_t fext = le16(blk, p + 6);
                if (fn1 == want_n1 && fn2 == want_n2 && fext == want_e) {
                    uint16_t acc = le16(blk, p + 12);
                    *out_retr_link = le16(blk, p + 14);
                    *out_size = 0;
                    if (acc) {
                        uint32_t alb;
                        size_t   ao;
                        uint8_t  abuf[RSTS_BLOCK_SIZE];
                        if (resolve_link(blk, dcs, acc, &alb, &ao) == 0 &&
                            ao + 16 <= RSTS_BLOCK_SIZE &&
                            read_block(fp, alb, abuf) == 0) {
                            uint16_t usiz  = le16(abuf, ao + 4);
                            uint16_t urts1 = le16(abuf, ao + 10);
                            uint16_t urts2 = le16(abuf, ao + 12);
                            *out_size = (urts1 == 0)
                                ? (((uint32_t)urts2 << 16) | usiz)
                                : (uint32_t)usiz;
                        }
                    }
                    return 0;
                }
            }
            if (next_link == prev) break;
            prev = cur;
            cur  = next_link;
        }
    }
    return -1;
}

/* Read SATT.SYS data (all blocks pointed to by its retrieval chain)
 * into a malloc'd buffer.  Caller frees.  *out_lbns gets the LBN of
 * each SATT block (so we can write back later).  Returns the number
 * of blocks read on success, 0 on any error. */
static uint32_t read_satt_data(FILE *fp, uint32_t ufd_lbn, uint32_t dcs,
                               uint16_t retr_link, uint32_t fcs,
                               uint32_t blocks, uint8_t **out_buf,
                               uint32_t **out_lbns) {
    uint8_t   blk[RSTS_BLOCK_SIZE];
    uint32_t  cur_lbn = ufd_lbn;
    uint16_t  cur = retr_link, prev = 0;
    int       guard;
    uint8_t  *buf;
    uint32_t *lbns;
    uint32_t  written = 0;
    if (fcs == 0) fcs = 1;
    if (blocks == 0) return 0;
    buf  = (uint8_t *)calloc(blocks, RSTS_BLOCK_SIZE);
    lbns = (uint32_t *)calloc(blocks, sizeof(uint32_t));
    if (!buf || !lbns) { free(buf); free(lbns); return 0; }
    if (read_block(fp, cur_lbn, blk) != 0) { free(buf); free(lbns); return 0; }
    for (guard = 0; cur != 0 && guard < 4096; guard++) {
        uint32_t tlbn;
        size_t   p;
        uint16_t next_link;
        int      i;
        if (resolve_link(blk, dcs, cur, &tlbn, &p) != 0) break;
        if (p + 16 > RSTS_BLOCK_SIZE) break;
        if (tlbn != cur_lbn) {
            if (read_block(fp, tlbn, blk) != 0) break;
            cur_lbn = tlbn;
        }
        next_link = le16(blk, p + 0);
        for (i = 0; i < 7 && written < blocks; i++) {
            uint16_t dcn = le16(blk, p + 2 + 2 * i);
            uint32_t k;
            if (dcn == 0) continue;
            for (k = 0; k < fcs && written < blocks; k++) {
                uint32_t lbn = (uint32_t)dcn * dcs + k;
                if (read_block(fp, lbn,
                               buf + (size_t)written * RSTS_BLOCK_SIZE) != 0) {
                    free(buf); free(lbns);
                    return 0;
                }
                lbns[written] = lbn;
                written++;
            }
        }
        if (next_link == prev) break;
        prev = cur; cur = next_link;
    }
    *out_buf  = buf;
    *out_lbns = lbns;
    return written;
}

/* Find a run of `count` consecutive 0 bits (PCNs).  bit_offset is the
 * starting PCN.  Bits are LSB-first within each byte. */
static long satt_find_free(const uint8_t *buf, uint32_t total_bytes,
                           uint32_t count) {
    uint32_t total_bits = total_bytes * 8;
    uint32_t i, run = 0, start = 0;
    if (count == 0) return -1;
    for (i = 0; i < total_bits; i++) {
        int bit = (buf[i >> 3] >> (i & 7)) & 1;
        if (bit == 0) {
            if (run == 0) start = i;
            run++;
            if (run >= count) return (long)start;
        } else {
            run = 0;
        }
    }
    return -1;
}

/* Find N free blockettes in the UFD (entries with first 4 bytes = 0).
 * Skips blockette 31 (cluster map) and blockette 0 (label).  Stops
 * when N have been found.  Out arrays receive (lbn, byte_offset) for
 * each.  Returns the number actually found.  Caller passes max_n =
 * size of arrays. */
static int find_free_blockettes(FILE *fp, uint32_t ufd_lbn, uint32_t dcs,
                                int max_n, uint32_t *out_lbn,
                                int *out_byte) {
    uint8_t  blk[RSTS_BLOCK_SIZE];
    uint32_t cur_lbn = ufd_lbn;
    int      found = 0;
    /* We walk every block that the UFD's cluster map references, in
     * order of cluster slot.  Within each block, scan blockettes 1..30
     * (skip 0=label/anchor and 31=cluster map). */
    if (read_block(fp, ufd_lbn, blk) != 0) return 0;
    {
        int      cl;
        uint32_t uclus_blocks;
        /* Cluster map: word 0 = cluster size in blocks, then 7 DCNs. */
        uclus_blocks = (uint32_t)le16(blk, 0x1F0);
        if (uclus_blocks == 0) uclus_blocks = 16;
        for (cl = 0; cl < 7 && found < max_n; cl++) {
            uint16_t dcn = le16(blk, 0x1F2 + 2 * cl);
            uint32_t b;
            if (dcn == 0) continue;
            for (b = 0; b < uclus_blocks && found < max_n; b++) {
                uint32_t lbn = (uint32_t)dcn * dcs + b;
                int      slot;
                /* Re-read cur block for this scan (different from blk[]
                 * which holds the first block of the UFD). */
                uint8_t  scan[RSTS_BLOCK_SIZE];
                if (read_block(fp, lbn, scan) != 0) continue;
                for (slot = 1; slot < 31 && found < max_n; slot++) {
                    int p = slot * 16;
                    /* Empty slot: first 4 bytes all zero. */
                    if (le16(scan, p) == 0 && le16(scan, p + 2) == 0) {
                        out_lbn[found]  = lbn;
                        out_byte[found] = p;
                        found++;
                    }
                }
            }
        }
        (void)cur_lbn;
    }
    return found;
}

/* Walk UFD Name Entry chain; return (lbn, byte_offset) of the entry
 * whose ULNK is 0 (the tail).  This is the entry to be patched when
 * inserting a new file.  Returns 0 on success. */
static int find_chain_tail(FILE *fp, uint32_t ufd_lbn, uint32_t dcs,
                           uint32_t *out_lbn, int *out_byte) {
    uint8_t  blk[RSTS_BLOCK_SIZE];
    uint32_t cur_lbn = ufd_lbn;
    uint16_t cur, prev_cur = 0, last_cur = 0;
    uint32_t last_lbn = 0;
    int      guard;
    if (read_block(fp, cur_lbn, blk) != 0) return -1;
    cur = le16(blk, 0);   /* link from label to first entry */
    if (cur == 0) {
        /* No entries yet; the label IS the tail. */
        *out_lbn  = ufd_lbn;
        *out_byte = 0;
        return 0;
    }
    for (guard = 0; cur != 0 && guard < 4096; guard++) {
        uint32_t tlbn;
        size_t   p;
        uint16_t next_link;
        if (resolve_link(blk, dcs, cur, &tlbn, &p) != 0) return -1;
        if (p + 16 > RSTS_BLOCK_SIZE) return -1;
        if (tlbn != cur_lbn) {
            if (read_block(fp, tlbn, blk) != 0) return -1;
            cur_lbn = tlbn;
        }
        next_link = le16(blk, p + 0);
        last_cur  = cur;
        last_lbn  = cur_lbn;
        if (next_link == 0) {
            *out_lbn  = cur_lbn;
            *out_byte = (int)p;
            return 0;
        }
        if (next_link == prev_cur) break;
        prev_cur = cur;
        cur      = next_link;
    }
    (void)last_cur; (void)last_lbn;
    return -1;
}

int rsts_copy_in(Mount *m, int g, int p,
                 const char *name, const char *type,
                 const char *host_path, int dry_run) {
    uint8_t      pkbuf[RSTS_BLOCK_SIZE];
    rsts_pack_t  pk;
    uint16_t     ppn = (uint16_t)(((g & 0xff) << 8) | (p & 0xff));
    uint16_t     uar_dcn = 0;
    uint32_t     ufd_lbn;
    struct stat  st;
    long long    fsize;
    uint32_t     blocks_needed;
    uint32_t     file_clusters;
    uint16_t     satt_retr;
    uint32_t     satt_size_blk;
    uint8_t     *satt_buf  = NULL;
    uint32_t    *satt_lbns = NULL;
    uint32_t     satt_blocks_read;
    long         pcn_start;
    uint32_t     blkt_lbn[3];
    int          blkt_byte[3];
    int          got_blkt;
    uint32_t     tail_lbn;
    int          tail_byte;
    uint16_t     fname_words[3];   /* word0,word1 + ext word */
    int          ret = -1;
    FILE        *in = NULL;

    if (!m || !m->fp || !name || !host_path) return -1;
    if (RT_FSEEK64(m->fp, (long long)RSTS_MFD_DCN *
                   rsts_dcs_for_size(m->total_blocks) * RSTS_BLOCK_SIZE,
                   SEEK_SET) != 0 ||
        fread(pkbuf, 1, sizeof(pkbuf), m->fp) != sizeof(pkbuf) ||
        !rsts_parse_pack_label(pkbuf, m->total_blocks, &pk)) {
        fprintf(stderr, "?'%s' is not a RSTS/E pack\n", m->path);
        return -1;
    }

    if (ppn == 0x0101) {
        ufd_lbn = pk.mfd_lbn;
    } else {
        if (find_account_uar(m->fp, pk.mfd_lbn, pk.dcs, ppn, &uar_dcn) != 0
            || uar_dcn == 0) {
            fprintf(stderr, "?Account [%d,%d] has no UFD\n", g, p);
            return -1;
        }
        ufd_lbn = (uint32_t)uar_dcn * pk.dcs;
    }

    if (stat(host_path, &st) != 0) {
        fprintf(stderr, "?Cannot stat host '%s'\n", host_path);
        return -1;
    }
    fsize         = (long long)st.st_size;
    blocks_needed = (uint32_t)((fsize + 511) / 512);
    if (blocks_needed == 0) blocks_needed = 1;
    /* FCS = PCS, so 1 file cluster = PCS blocks. */
    file_clusters = (blocks_needed + pk.pcs - 1) / pk.pcs;
    if (file_clusters > 7) {
        fprintf(stderr, "?File too large for single Retrieval Entry "
                "(%u clusters > 7); not yet supported\n",
                (unsigned)file_clusters);
        return -1;
    }

    fname_words[0] = rad50_encode_word(name, 3);
    fname_words[1] = rad50_encode_word(strlen(name) > 3 ? name + 3 : "", 3);
    fname_words[2] = rad50_encode_word(type ? type : "", 3);

    printf("RSTS/E COPY IN%s:\n", dry_run ? "  [DRY RUN]" : "");
    printf("  Source : %s  (%lld bytes, %u blocks)\n",
           host_path, fsize, (unsigned)blocks_needed);
    printf("  Target : [%d,%d]%s.%s\n", g, p, name, type ? type : "");
    printf("  Pack   : %s  PCS=%u DCS=%u  UFD@LBN=%u\n",
           pk.pack_id, (unsigned)pk.pcs, (unsigned)pk.dcs,
           (unsigned)ufd_lbn);
    printf("  File clusters: %u  (FCS=PCS=%u)\n",
           (unsigned)file_clusters, (unsigned)pk.pcs);

    /* Locate SATT.SYS. */
    if (find_satt_info(m->fp, pk.mfd_lbn, pk.dcs,
                       &satt_retr, &satt_size_blk) != 0) {
        fprintf(stderr, "?Cannot locate SATT.SYS in [0,1]\n");
        return -1;
    }
    if (satt_size_blk == 0) {
        fprintf(stderr, "?SATT.SYS reports size 0 - cannot allocate\n");
        return -1;
    }
    /* Walk [0,1]'s UFD area to read SATT data. */
    {
        uint16_t a01_uar = 0;
        uint32_t a01_lbn;
        if (find_account_uar(m->fp, pk.mfd_lbn, pk.dcs,
                             0x0001, &a01_uar) != 0 || a01_uar == 0) {
            fprintf(stderr, "?Cannot find [0,1] for SATT data read\n");
            return -1;
        }
        a01_lbn = (uint32_t)a01_uar * pk.dcs;
        satt_blocks_read = read_satt_data(m->fp, a01_lbn, pk.dcs,
                                          satt_retr, pk.pcs,
                                          satt_size_blk,
                                          &satt_buf, &satt_lbns);
        if (satt_blocks_read == 0) {
            fprintf(stderr, "?SATT.SYS read failed\n");
            return -1;
        }
    }

    pcn_start = satt_find_free(satt_buf,
                               satt_blocks_read * RSTS_BLOCK_SIZE,
                               file_clusters);
    if (pcn_start < 0) {
        fprintf(stderr, "?No contiguous run of %u free PCNs in SATT.SYS\n",
                (unsigned)file_clusters);
        free(satt_buf); free(satt_lbns); return -1;
    }

    /* Convert PCN_start to DCN_start.  PCN 0 starts at DCN 1, and
     * each PCN spans (PCS/DCS) DCNs. */
    {
        uint32_t dcn_start = 1u + (uint32_t)pcn_start * (pk.pcs / pk.dcs);
        uint32_t lbn_start = dcn_start * pk.dcs;
        printf("  SATT  : PCN run [%ld..%ld]  -> DCN_start=%u  LBN_start=%u\n",
               pcn_start, pcn_start + (long)file_clusters - 1,
               (unsigned)dcn_start, (unsigned)lbn_start);

        /* Find 3 free blockettes in UFD. */
        got_blkt = find_free_blockettes(m->fp, ufd_lbn, pk.dcs,
                                        3, blkt_lbn, blkt_byte);
        if (got_blkt < 3) {
            fprintf(stderr,
                    "?UFD [%d,%d] doesn't have 3 free blockettes "
                    "(found %d); UFD-extend not yet supported\n",
                    g, p, got_blkt);
            free(satt_buf); free(satt_lbns); return -1;
        }
        printf("  UFD blockettes:\n");
        printf("    Name   @LBN=%u byte=%d\n", (unsigned)blkt_lbn[0], blkt_byte[0]);
        printf("    Acc    @LBN=%u byte=%d\n", (unsigned)blkt_lbn[1], blkt_byte[1]);
        printf("    Retr   @LBN=%u byte=%d\n", (unsigned)blkt_lbn[2], blkt_byte[2]);

        if (find_chain_tail(m->fp, ufd_lbn, pk.dcs,
                            &tail_lbn, &tail_byte) != 0) {
            fprintf(stderr, "?Cannot find Name Entry chain tail in UFD\n");
            free(satt_buf); free(satt_lbns); return -1;
        }
        printf("  Chain tail @LBN=%u byte=%d  (will get new ULNK)\n",
               (unsigned)tail_lbn, tail_byte);

        if (dry_run) {
            printf("\n  [DRY RUN] no writes made.\n");
            printf("  Would do:\n"
                   "    1) Set %u SATT.SYS bits starting at PCN %ld\n"
                   "    2) Write Name Entry blockette at LBN=%u byte=%d\n"
                   "    3) Write Accounting Entry at LBN=%u byte=%d\n"
                   "    4) Write Retrieval Entry at LBN=%u byte=%d\n"
                   "    5) Patch tail ULNK at LBN=%u byte=%d\n"
                   "    6) Write %u data blocks at LBN=%u..%u\n",
                   (unsigned)file_clusters, pcn_start,
                   (unsigned)blkt_lbn[0], blkt_byte[0],
                   (unsigned)blkt_lbn[1], blkt_byte[1],
                   (unsigned)blkt_lbn[2], blkt_byte[2],
                   (unsigned)tail_lbn, tail_byte,
                   (unsigned)blocks_needed,
                   (unsigned)lbn_start,
                   (unsigned)(lbn_start + blocks_needed - 1));
            free(satt_buf); free(satt_lbns); return 0;
        }

        /* === REAL WRITES === */

        /* Compute in-cluster block / cluster index for the 3 UFD
         * blockettes.  We need them as link words pointing TO each. */
        {
            uint32_t name_lbn = blkt_lbn[0];
            uint32_t acc_lbn  = blkt_lbn[1];
            uint32_t retr_lbn = blkt_lbn[2];
            int      name_byte = blkt_byte[0];
            int      acc_byte  = blkt_byte[1];
            int      retr_byte = blkt_byte[2];
            uint16_t name_link, acc_link, retr_link;
            uint8_t  ufd_blk[RSTS_BLOCK_SIZE];

            /* Convert (lbn, byte) -> (block_in_cluster, blockette).
             * Here we assume the new blockettes live in the FIRST
             * cluster of the UFD (cluster_idx=0).  This is true when
             * find_free_blockettes scans cluster 0 first, which it
             * does in our implementation. */
            int name_block_in_cluster = (int)(name_lbn - ufd_lbn);
            int acc_block_in_cluster  = (int)(acc_lbn  - ufd_lbn);
            int retr_block_in_cluster = (int)(retr_lbn - ufd_lbn);

            name_link = make_link(name_block_in_cluster, 0,
                                  name_byte / 16, 0);
            acc_link  = make_link(acc_block_in_cluster,  0,
                                  acc_byte  / 16, 1);
            retr_link = make_link(retr_block_in_cluster, 0,
                                  retr_byte / 16, 1);

            /* 1) Update SATT.SYS bits and write back affected blocks. */
            {
                uint32_t b;
                /* Set bits */
                for (b = 0; b < file_clusters; b++) {
                    uint32_t bit = (uint32_t)pcn_start + b;
                    satt_buf[bit >> 3] |= (uint8_t)(1u << (bit & 7));
                }
                /* Find which SATT block(s) were modified and write back. */
                for (b = 0; b < satt_blocks_read; b++) {
                    /* For simplicity write all SATT blocks that contain
                     * any of our affected bits. */
                    uint32_t first_bit = b * RSTS_BLOCK_SIZE * 8;
                    uint32_t last_bit  = first_bit + RSTS_BLOCK_SIZE * 8 - 1;
                    if (first_bit > (uint32_t)(pcn_start + file_clusters - 1) ||
                        last_bit  < (uint32_t)pcn_start) continue;
                    if (RT_FSEEK64(m->fp,
                                   (long long)satt_lbns[b] * 512LL,
                                   SEEK_SET) != 0 ||
                        fwrite(satt_buf + b * RSTS_BLOCK_SIZE, 1,
                               RSTS_BLOCK_SIZE, m->fp) != RSTS_BLOCK_SIZE) {
                        fprintf(stderr, "?SATT write failed at LBN=%u\n",
                                (unsigned)satt_lbns[b]);
                        free(satt_buf); free(satt_lbns); return -1;
                    }
                }
            }

            /* 2) Build + write Name Entry blockette. */
            if (read_block(m->fp, name_lbn, ufd_blk) != 0) goto bail;
            le16_write(ufd_blk, name_byte + 0,  0);              /* ULNK = 0 (we are tail) */
            le16_write(ufd_blk, name_byte + 2,  fname_words[0]); /* FILNAM word 1 */
            le16_write(ufd_blk, name_byte + 4,  fname_words[1]); /* FILNAM word 2 */
            le16_write(ufd_blk, name_byte + 6,  fname_words[2]); /* EXT */
            ufd_blk[name_byte + 8]  = 0;                          /* USTAT */
            ufd_blk[name_byte + 9]  = 0060;                       /* UPROT octal */
            le16_write(ufd_blk, name_byte + 10, 0);               /* UACNT */
            le16_write(ufd_blk, name_byte + 12, acc_link);
            le16_write(ufd_blk, name_byte + 14, retr_link);
            if (RT_FSEEK64(m->fp, (long long)name_lbn * 512LL, SEEK_SET) != 0 ||
                fwrite(ufd_blk, 1, RSTS_BLOCK_SIZE, m->fp) != RSTS_BLOCK_SIZE)
                goto bail;

            /* 3) Build + write Accounting Entry blockette. */
            if (read_block(m->fp, acc_lbn, ufd_blk) != 0) goto bail;
            le16_write(ufd_blk, acc_byte + 0,  0);                  /* ULNK to attr */
            le16_write(ufd_blk, acc_byte + 2,  today_rsts_date());  /* UDLA */
            le16_write(ufd_blk, acc_byte + 4,  (uint16_t)blocks_needed); /* USIZ */
            le16_write(ufd_blk, acc_byte + 6,  today_rsts_date());  /* UDC */
            le16_write(ufd_blk, acc_byte + 8,  now_rsts_time());    /* UTC */
            le16_write(ufd_blk, acc_byte + 10, 0);                  /* URTS w1 (large file convention -> size MSB next) */
            le16_write(ufd_blk, acc_byte + 12, (uint16_t)((uint64_t)blocks_needed >> 16)); /* URTS w2 = size MSB */
            le16_write(ufd_blk, acc_byte + 14, (uint16_t)pk.pcs);   /* UCLUS = FCS */
            if (RT_FSEEK64(m->fp, (long long)acc_lbn * 512LL, SEEK_SET) != 0 ||
                fwrite(ufd_blk, 1, RSTS_BLOCK_SIZE, m->fp) != RSTS_BLOCK_SIZE)
                goto bail;

            /* 4) Build + write Retrieval Entry blockette. */
            if (read_block(m->fp, retr_lbn, ufd_blk) != 0) goto bail;
            le16_write(ufd_blk, retr_byte + 0, 0); /* ULNK = 0 (single retrieval) */
            {
                int i;
                uint32_t dcn0 = 1u + (uint32_t)pcn_start * (pk.pcs / pk.dcs);
                for (i = 0; i < 7; i++) {
                    if ((uint32_t)i < file_clusters) {
                        uint16_t dcn = (uint16_t)(dcn0 +
                                                  (uint32_t)i * (pk.pcs / pk.dcs));
                        le16_write(ufd_blk, retr_byte + 2 + 2 * i, dcn);
                    } else {
                        le16_write(ufd_blk, retr_byte + 2 + 2 * i, 0);
                    }
                }
            }
            if (RT_FSEEK64(m->fp, (long long)retr_lbn * 512LL, SEEK_SET) != 0 ||
                fwrite(ufd_blk, 1, RSTS_BLOCK_SIZE, m->fp) != RSTS_BLOCK_SIZE)
                goto bail;

            /* 5) Patch the previous chain-tail's ULNK to point at our
             *    new Name Entry. */
            if (read_block(m->fp, tail_lbn, ufd_blk) != 0) goto bail;
            le16_write(ufd_blk, tail_byte + 0, name_link);
            if (RT_FSEEK64(m->fp, (long long)tail_lbn * 512LL, SEEK_SET) != 0 ||
                fwrite(ufd_blk, 1, RSTS_BLOCK_SIZE, m->fp) != RSTS_BLOCK_SIZE)
                goto bail;

            /* 6) Stream host data to allocated LBNs. */
            in = fopen(host_path, "rb");
            if (!in) {
                fprintf(stderr, "?Cannot open '%s'\n", host_path);
                goto bail;
            }
            {
                uint32_t i;
                uint8_t  buf[RSTS_BLOCK_SIZE];
                for (i = 0; i < blocks_needed; i++) {
                    size_t got;
                    memset(buf, 0, sizeof(buf));
                    got = fread(buf, 1, RSTS_BLOCK_SIZE, in);
                    if (RT_FSEEK64(m->fp,
                                   (long long)(lbn_start + i) * 512LL,
                                   SEEK_SET) != 0 ||
                        fwrite(buf, 1, RSTS_BLOCK_SIZE, m->fp)
                            != RSTS_BLOCK_SIZE) {
                        fprintf(stderr, "?Data write failed at LBN=%u\n",
                                (unsigned)(lbn_start + i));
                        fclose(in); in = NULL;
                        goto bail;
                    }
                    (void)got;
                }
            }
            fclose(in); in = NULL;

            fflush(m->fp);
            printf("Copied '%s' -> [%d,%d]%s.%s  "
                   "(%u blocks at LBN=%u)\n",
                   host_path, g, p, name, type ? type : "",
                   (unsigned)blocks_needed, (unsigned)lbn_start);
            ret = 0;
        }
    }

bail:
    if (in) fclose(in);
    free(satt_buf); free(satt_lbns);
    return ret;
}
