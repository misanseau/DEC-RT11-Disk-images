/*
 * mt.c - RT-11 File-Structured Magtape (FSM) operations over the
 *        SIMH .tap container format.
 *
 * Tape layout (DEC RT-11 V&FF manual section 1.2.1.1):
 *
 *   VOL1
 *   HDR1 *
 *   data...
 *   *
 *   EOF1 *
 *   [HDR1 ... EOF1 * ]*
 *   * *                  (double tape mark = logical EOT)
 *
 * Container (SIMH .tap):
 *   <pre-len u32-LE> <data N bytes> <pad 0/1> <post-len u32-LE>
 *   tape mark: <u32-LE = 0>
 */
#include "mt.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_MSC_VER)
#  include <io.h>
#  define mt_truncate(fd,len)  (_chsize_s((fd),(len)) == 0 ? 0 : -1)
#  define mt_fileno            _fileno
#else
#  include <unistd.h>
#  define mt_truncate(fd,len)  (ftruncate((fd),(len)) == 0 ? 0 : -1)
#  define mt_fileno            fileno
#endif

/* --- Low-level .tap framing -------------------------------------------- */

static int mt_read_u32(FILE *fp, uint32_t *out) {
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) return -1;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return 0;
}

static int mt_write_u32(FILE *fp, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v       & 0xff);
    b[1] = (uint8_t)((v>>8)  & 0xff);
    b[2] = (uint8_t)((v>>16) & 0xff);
    b[3] = (uint8_t)((v>>24) & 0xff);
    return fwrite(b, 1, 4, fp) == 4 ? 0 : -1;
}

/* Read the next tape event.  Returns:
 *    0 = a data record was consumed (contents in buf, length in *actual)
 *    1 = a tape mark was consumed
 *   -1 = I/O error or end of medium (also returned if record is bigger
 *        than the buffer, after skipping). */
static int mt_read_rec(FILE *fp, void *buf, uint32_t bufsz, uint32_t *actual) {
    uint32_t pre, post;
    if (mt_read_u32(fp, &pre) != 0) return -1;
    if (pre == MT_TAPE_MARK) { *actual = 0; return 1; }
    if (pre == MT_EOM)       { *actual = 0; return -1; }
    if (pre > bufsz) {
        long skip = (long)(pre + (pre & 1u) + 4u);
        if (fseek(fp, skip, SEEK_CUR) != 0) return -1;
        *actual = pre;
        return -1;
    }
    if (fread(buf, 1, pre, fp) != pre) return -1;
    if (pre & 1u) {
        uint8_t pad;
        if (fread(&pad, 1, 1, fp) != 1) return -1;
    }
    if (mt_read_u32(fp, &post) != 0) return -1;
    (void)post;   /* nominally == pre; some tools accept a mismatch */
    *actual = pre;
    return 0;
}

static int mt_write_rec(FILE *fp, const void *buf, uint32_t len) {
    if (mt_write_u32(fp, len) != 0) return -1;
    if (len > 0 && fwrite(buf, 1, len, fp) != len) return -1;
    if (len & 1u) {
        uint8_t pad = 0;
        if (fwrite(&pad, 1, 1, fp) != 1) return -1;
    }
    if (mt_write_u32(fp, len) != 0) return -1;
    return 0;
}

static int mt_write_tape_mark(FILE *fp) {
    return mt_write_u32(fp, MT_TAPE_MARK);
}

/* --- Label helpers ----------------------------------------------------- */

/* Write up to `length` chars of `s` into label at 1-based position pos.
 * Pads with spaces if s is shorter than length. NULL s = all spaces. */
static void mt_put(char lab[MT_LABEL_BYTES], int pos1, int length, const char *s) {
    int i;
    int plen = s ? (int)strlen(s) : 0;
    if (plen > length) plen = length;
    for (i = 0; i < length; i++) {
        lab[pos1 - 1 + i] = (i < plen) ? s[i] : ' ';
    }
}

static void mt_put_u(char lab[MT_LABEL_BYTES], int pos1, int length,
                     uint32_t v, int zero_pad)
{
    char tmp[16];
    int n;
    if (zero_pad) n = snprintf(tmp, sizeof(tmp), "%0*u", length, (unsigned)v);
    else          n = snprintf(tmp, sizeof(tmp), "%*u",  length, (unsigned)v);
    if (n < 0) n = 0;
    if (n > length) n = length;
    mt_put(lab, pos1, length, tmp);
    (void)n;
}

/* Encode today's date as " YYDDD" where YY=year%100 and DDD=day-of-year. */
static void mt_date_today(char out[6]) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    int yr2 = (tm ? tm->tm_year : 0) % 100;
    int doy = (tm ? tm->tm_yday : 0) + 1;
    char tmp[8];
    snprintf(tmp, sizeof(tmp), " %02d%03d", yr2, doy);
    memcpy(out, tmp, 6);
}

/* Fill a VOL1 label.  vol_id must be up to 6 chars; owner up to 10. */
static void mt_fill_vol1(char lab[MT_LABEL_BYTES],
                         const char *vol_id, const char *owner_name)
{
    int i;
    for (i = 0; i < MT_LABEL_BYTES; i++) lab[i] = ' ';
    mt_put(lab, 1,  4, "VOL1");
    mt_put(lab, 5,  6, (vol_id && *vol_id) ? vol_id : "RT11A");
    /* 11: accessibility = space */
    /* 12-37: reserved spaces */
    mt_put(lab, 38, 3, "D%B");
    mt_put(lab, 41, 10, (owner_name && *owner_name) ? owner_name : "");
    mt_put(lab, 51, 1, "1");
    /* 52-79: reserved spaces */
    mt_put(lab, 80, 1, "3");
}

static void mt_fill_hdr_or_eof(char lab[MT_LABEL_BYTES],
                               int is_eof,
                               const char *tape_name_dot_ext,
                               uint32_t file_sequence,
                               uint32_t block_count)
{
    int i;
    char datebuf[6];
    for (i = 0; i < MT_LABEL_BYTES; i++) lab[i] = ' ';
    mt_put(lab, 1, 4, is_eof ? "EOF1" : "HDR1");
    /* 5-21: file identifier, 17 chars, NN.E style left-justified */
    mt_put(lab, 5, 17, tape_name_dot_ext);
    /* 22-27: file set identifier */
    mt_put(lab, 22, 6, "RT11A");
    /* 28-31: file section number */
    mt_put(lab, 28, 4, "0001");
    /* 32-35: file sequence number (decimal, zero-padded) */
    mt_put_u(lab, 32, 4, file_sequence, 1);
    /* 36-39: generation number */
    mt_put(lab, 36, 4, "0001");
    /* 40-41: generation version */
    mt_put(lab, 40, 2, "00");
    /* 42-47: creation date " YYDDD" */
    mt_date_today(datebuf);
    for (i = 0; i < 6; i++) lab[41 + i] = datebuf[i];
    /* 48-53: expiration date - no date */
    mt_put(lab, 48, 6, " 00000");
    /* 54: accessibility */
    /* 55-60: block count */
    mt_put_u(lab, 55, 6, is_eof ? block_count : 0u, 1);
    /* 61-73: system code */
    mt_put(lab, 61, 13, "DECRT11A");
    /* 74-80: reserved spaces */
}

static uint32_t mt_parse_u(const char *s, int len) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') v = v*10 + (uint32_t)(c - '0');
    }
    return v;
}

/* Extract filename from HDR1/EOF1 label (positions 5-21). Returns pointer
 * to *out, NUL-terminated, trimmed of trailing spaces. */
static void mt_extract_filename(const char lab[MT_LABEL_BYTES], char out[18]) {
    int k;
    memcpy(out, lab + 4, 17);
    out[17] = '\0';
    for (k = 16; k >= 0 && out[k] == ' '; k--) out[k] = '\0';
}

/* Extract "RT-11 year + day-of-year" from HDR1 label positions 42-47
 * (" YYDDD").  *y, *doy are set to 0 if not parseable. */
static void mt_extract_date(const char lab[MT_LABEL_BYTES], int *y, int *doy) {
    uint32_t v = mt_parse_u(lab + 41 + 1, 5);   /* skip the leading space */
    if (v == 0) { *y = 0; *doy = 0; return; }
    *doy = (int)(v % 1000);
    *y   = (int)(v / 1000);
}

static void mt_doy_to_md(int y, int doy, int *m, int *d) {
    static const int days[2][12] = {
        {31,28,31,30,31,30,31,31,30,31,30,31},
        {31,29,31,30,31,30,31,31,30,31,30,31}
    };
    int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 1 : 0;
    int i;
    int d2 = doy;
    for (i = 0; i < 12; i++) {
        if (d2 <= days[leap][i]) { *m = i + 1; *d = d2; return; }
        d2 -= days[leap][i];
    }
    *m = 0; *d = 0;
}

/* --- Tape volume scan -------------------------------------------------- */

#define MT_MAX_FILES 128

typedef struct {
    long long hdr1_pos;   /* pre-len offset of HDR1 record */
    long long data_pos;   /* pre-len offset of first data record (or tape mark if empty) */
    long long eof1_pos;   /* pre-len offset of EOF1 record */
    long long next_pos;   /* pre-len offset of the byte right after the tape mark that closes EOF1 */
    char      hdr1[MT_LABEL_BYTES];
    char      filename[18];
    uint32_t  block_count;
} MtFileEntry;

typedef struct {
    char        vol1[MT_LABEL_BYTES];
    long long   eot_pos;            /* position at which the logical-EOT
                                     * double tape mark begins — where
                                     * COPY-in rewrites new files. */
    int         nfiles;
    MtFileEntry files[MT_MAX_FILES];
} MtVolume;

static int mt_scan(Mount *m, MtVolume *vol) {
    FILE *fp = m->fp;
    char buf[MT_RECORD_MAX];
    uint32_t actual;
    int rc;

    memset(vol, 0, sizeof(*vol));

    if (RT_FSEEK64(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "?Cannot rewind tape\n");
        return -1;
    }

    /* VOL1 */
    rc = mt_read_rec(fp, buf, sizeof(buf), &actual);
    if (rc != 0 || actual != MT_LABEL_BYTES || memcmp(buf, "VOL1", 4) != 0) {
        fprintf(stderr, "?Tape has no VOL1 label (use FORMAT %s first)\n",
                m->path);
        return -1;
    }
    memcpy(vol->vol1, buf, MT_LABEL_BYTES);

    for (;;) {
        long long pos_before;
        MtFileEntry *fe;

        pos_before = RT_FTELL64(fp);
        rc = mt_read_rec(fp, buf, sizeof(buf), &actual);
        if (rc == 1) {
            /* A tape mark immediately where we expected HDR1.
             * Per the spec, the "EOF1 *" of the previous file is a group,
             * and the logical EOT is a double tape mark.  Seeing one
             * tape mark here means we are reading the first of the two
             * EOT tape marks.  Check for the second. */
            long long pos_second = RT_FTELL64(fp);
            rc = mt_read_rec(fp, buf, sizeof(buf), &actual);
            if (rc == 1) {
                vol->eot_pos = pos_before;
                return 0;
            }
            (void)pos_second;
            fprintf(stderr, "?Malformed tape: expected HDR1 or EOT\n");
            return -1;
        }
        if (rc != 0) {
            fprintf(stderr, "?I/O error scanning tape\n");
            return -1;
        }
        if (actual != MT_LABEL_BYTES || memcmp(buf, "HDR1", 4) != 0) {
            fprintf(stderr, "?Expected HDR1 on tape at offset %lld\n",
                    pos_before);
            return -1;
        }

        if (vol->nfiles >= MT_MAX_FILES) {
            fprintf(stderr, "?Too many files on tape (limit %d)\n",
                    MT_MAX_FILES);
            return -1;
        }
        fe = &vol->files[vol->nfiles++];
        fe->hdr1_pos = pos_before;
        memcpy(fe->hdr1, buf, MT_LABEL_BYTES);
        mt_extract_filename(fe->hdr1, fe->filename);

        /* Tape mark after HDR1. */
        rc = mt_read_rec(fp, buf, sizeof(buf), &actual);
        if (rc != 1) {
            fprintf(stderr, "?Missing tape mark after HDR1 '%s'\n",
                    fe->filename);
            return -1;
        }

        /* Data blocks until next tape mark. */
        fe->data_pos    = RT_FTELL64(fp);
        fe->block_count = 0;
        for (;;) {
            rc = mt_read_rec(fp, buf, sizeof(buf), &actual);
            if (rc == 1) break;
            if (rc != 0) {
                fprintf(stderr, "?I/O error in data for '%s'\n", fe->filename);
                return -1;
            }
            fe->block_count++;
        }

        /* EOF1 */
        fe->eof1_pos = RT_FTELL64(fp);
        rc = mt_read_rec(fp, buf, sizeof(buf), &actual);
        if (rc != 0 || actual != MT_LABEL_BYTES || memcmp(buf, "EOF1", 4) != 0) {
            fprintf(stderr, "?Missing EOF1 after data for '%s'\n",
                    fe->filename);
            return -1;
        }

        /* Tape mark after EOF1. */
        rc = mt_read_rec(fp, buf, sizeof(buf), &actual);
        if (rc != 1) {
            fprintf(stderr, "?Missing tape mark after EOF1 '%s'\n",
                    fe->filename);
            return -1;
        }
        fe->next_pos = RT_FTELL64(fp);
    }
}

/* --- Public operations ------------------------------------------------- */

int mt_format(Mount *m, const char *vol_id, const char *owner_name) {
    FILE *fp;
    char vol1[MT_LABEL_BYTES];
    char hdr[MT_LABEL_BYTES];
    char eof[MT_LABEL_BYTES];
    long long end_pos;

    if (!m || m->kind != MOUNT_KIND_MT) {
        fprintf(stderr, "?FORMAT on tape: mount is not a tape\n");
        return -1;
    }
    fp = m->fp;

    mt_fill_vol1 (vol1, vol_id,      owner_name);
    mt_fill_hdr_or_eof(hdr, 0, "",   0, 0);
    mt_fill_hdr_or_eof(eof, 1, "",   0, 0);

    if (RT_FSEEK64(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "?Cannot rewind tape\n");
        return -1;
    }

    /* Per spec, an initialised tape has:
     *   VOL1 HDR1 * * EOF1 * * *
     *
     * That is: VOL1 label, HDR1 placeholder, tape mark,
     *          (no data), tape mark, EOF1 placeholder, tape mark,
     *          then two more tape marks for logical EOT. */
    if (mt_write_rec(fp, vol1, MT_LABEL_BYTES) != 0) return -1;
    if (mt_write_rec(fp, hdr,  MT_LABEL_BYTES) != 0) return -1;
    if (mt_write_tape_mark(fp) != 0) return -1;
    if (mt_write_tape_mark(fp) != 0) return -1;
    if (mt_write_rec(fp, eof,  MT_LABEL_BYTES) != 0) return -1;
    if (mt_write_tape_mark(fp) != 0) return -1;
    if (mt_write_tape_mark(fp) != 0) return -1;
    if (mt_write_tape_mark(fp) != 0) return -1;
    fflush(fp);

    end_pos = RT_FTELL64(fp);
    if (end_pos >= 0) {
        if (mt_truncate(mt_fileno(fp), end_pos) != 0) {
            /* Not fatal if truncate fails; file just has trailing bytes. */
        }
        m->size_bytes = end_pos;
    }
    return 0;
}

int mt_cmd_dir(Mount *m) {
    MtVolume vol;
    char vol_id[7];
    char owner[11];
    int i;
    uint32_t total_blocks = 0;

    if (!m || m->kind != MOUNT_KIND_MT) return -1;
    if (mt_scan(m, &vol) != 0) return -1;

    /* VOL1 positions 5-10 = volume id, 41-50 = owner (from owner ident). */
    memcpy(vol_id, vol.vol1 + 4, 6); vol_id[6] = '\0';
    memcpy(owner,  vol.vol1 + 40, 10); owner[10] = '\0';

    printf(" Volume ID: %-6s (MT tape)\n", vol_id);
    printf(" Owner    : %-10s\n", owner);
    printf("\n");

    if (vol.nfiles == 0) {
        printf(" (empty tape)\n");
    } else {
        for (i = 0; i < vol.nfiles; i++) {
            MtFileEntry *fe = &vol.files[i];
            int y = 0, doy = 0, mn = 0, dy = 0;
            char datestr[16];
            mt_extract_date(fe->hdr1, &y, &doy);
            if (y > 0) {
                static const char *mname[] = {
                    "Jan","Feb","Mar","Apr","May","Jun",
                    "Jul","Aug","Sep","Oct","Nov","Dec" };
                mt_doy_to_md(1900 + y, doy, &mn, &dy);
                if (mn >= 1 && mn <= 12) {
                    int full_y = (y >= 72) ? 1900 + y : 2000 + y;
                    snprintf(datestr, sizeof(datestr), "%02d-%s-%d",
                             dy, mname[mn-1], full_y);
                } else {
                    snprintf(datestr, sizeof(datestr), "           ");
                }
            } else {
                snprintf(datestr, sizeof(datestr), "           ");
            }
            if (fe->filename[0] == '\0') continue;  /* skip placeholder */
            printf(" %-12s %5u  %s\n",
                   fe->filename, (unsigned)fe->block_count, datestr);
            total_blocks += fe->block_count;
        }
    }

    {
        int real = 0, k;
        for (k = 0; k < vol.nfiles; k++) {
            if (vol.files[k].filename[0]) real++;
        }
        printf("\n %d Files, %u Blocks\n", real, (unsigned)total_blocks);
    }
    return 0;
}

int mt_copy_out(Mount *m, const char *tape_name, const char *host_path) {
    MtVolume vol;
    int i;
    MtFileEntry *fe = NULL;
    FILE *out;
    char buf[MT_BLOCK_BYTES];
    uint32_t actual;
    int rc;

    if (!m || m->kind != MOUNT_KIND_MT) return -1;
    if (!tape_name || !*tape_name) {
        fprintf(stderr, "?Missing tape filename\n");
        return -1;
    }
    if (mt_scan(m, &vol) != 0) return -1;

    for (i = 0; i < vol.nfiles; i++) {
        if (vol.files[i].filename[0] == '\0') continue;
        if (strcieq(vol.files[i].filename, tape_name)) {
            fe = &vol.files[i];
            break;
        }
    }
    if (!fe) {
        fprintf(stderr, "?File '%s' not on tape\n", tape_name);
        return -1;
    }

    out = fopen(host_path, "wb");
    if (!out) {
        fprintf(stderr, "?Cannot create host file '%s': %s\n",
                host_path, strerror(errno));
        return -1;
    }

    if (RT_FSEEK64(m->fp, fe->data_pos, SEEK_SET) != 0) {
        fprintf(stderr, "?Cannot seek to data position\n");
        fclose(out);
        return -1;
    }
    for (;;) {
        rc = mt_read_rec(m->fp, buf, sizeof(buf), &actual);
        if (rc == 1) break;   /* tape mark = end of file */
        if (rc != 0) {
            fprintf(stderr, "?Read error extracting '%s'\n", tape_name);
            fclose(out);
            return -1;
        }
        if (fwrite(buf, 1, actual, out) != actual) {
            fprintf(stderr, "?Write error on '%s'\n", host_path);
            fclose(out);
            return -1;
        }
    }
    fclose(out);
    printf("Copied %s -> '%s' (%u blocks)\n",
           tape_name, host_path, (unsigned)fe->block_count);
    return 0;
}

/* Produce an uppercase RT-11 style "NNNNNN.EEE" from a host basename. */
static void mt_derive_name(const char *host_path, char out[18]) {
    const char *p = host_path;
    const char *s1 = strrchr(p, '/');
    const char *s2 = strrchr(p, '\\');
    const char *base = p;
    const char *dot;
    char name[11] = {0};
    char ext[4]   = {0};
    int  i, j;
    if (s1 && (!s2 || s1 > s2)) base = s1 + 1;
    else if (s2)                base = s2 + 1;

    dot = strrchr(base, '.');
    for (i = 0, j = 0; base[i] && (!dot || base + i < dot) && j < 6; i++) {
        if (isalnum((unsigned char)base[i])) name[j++] = (char)toupper((unsigned char)base[i]);
    }
    if (dot) {
        for (i = 1, j = 0; dot[i] && j < 3; i++) {
            if (isalnum((unsigned char)dot[i])) ext[j++] = (char)toupper((unsigned char)dot[i]);
        }
    }
    if (ext[0]) snprintf(out, 18, "%s.%s", name, ext);
    else        snprintf(out, 18, "%s",    name);
}

int mt_copy_in(Mount *m, const char *host_path, const char *tape_name) {
    MtVolume vol;
    FILE *in;
    char hdr[MT_LABEL_BYTES];
    char eof[MT_LABEL_BYTES];
    char block[MT_BLOCK_BYTES];
    char rt11_name[18];
    uint32_t file_seq;
    uint32_t block_count = 0;
    size_t rd;
    long long end_pos;

    if (!m || m->kind != MOUNT_KIND_MT) return -1;

    if (mt_scan(m, &vol) != 0) return -1;

    if (tape_name && *tape_name) {
        strlcopy(rt11_name, tape_name, sizeof(rt11_name));
        strupper(rt11_name);
    } else {
        mt_derive_name(host_path, rt11_name);
    }
    if (!rt11_name[0]) {
        fprintf(stderr, "?Cannot derive RT-11 filename from '%s'\n", host_path);
        return -1;
    }

    in = fopen(host_path, "rb");
    if (!in) {
        fprintf(stderr, "?Cannot open host file '%s': %s\n",
                host_path, strerror(errno));
        return -1;
    }

    /* Seek to the logical EOT position and overwrite the double tape mark
     * with the new file's HDR1 / data / EOF1 followed by a fresh EOT. */
    if (RT_FSEEK64(m->fp, vol.eot_pos, SEEK_SET) != 0) {
        fprintf(stderr, "?Cannot seek to tape EOT\n");
        fclose(in);
        return -1;
    }

    /* File sequence = nfiles_real + 1 (placeholders from init have
     * filename[0]=='\0' and are renumbered as we add the first real file). */
    {
        int k;
        int real = 0;
        for (k = 0; k < vol.nfiles; k++) {
            if (vol.files[k].filename[0]) real++;
        }
        file_seq = (uint32_t)(real + 1);
    }

    mt_fill_hdr_or_eof(hdr, 0, rt11_name, file_seq, 0);
    if (mt_write_rec(m->fp, hdr, MT_LABEL_BYTES) != 0) goto io_err;
    if (mt_write_tape_mark(m->fp) != 0)                goto io_err;

    /* Copy data in 512-byte records, padding the last with zero bytes. */
    for (;;) {
        rd = fread(block, 1, MT_BLOCK_BYTES, in);
        if (rd == 0) break;
        if (rd < MT_BLOCK_BYTES) {
            memset(block + rd, 0, MT_BLOCK_BYTES - rd);
        }
        if (mt_write_rec(m->fp, block, MT_BLOCK_BYTES) != 0) goto io_err;
        block_count++;
        if (rd < MT_BLOCK_BYTES) break;
    }
    fclose(in); in = NULL;

    if (mt_write_tape_mark(m->fp) != 0) goto io_err;

    mt_fill_hdr_or_eof(eof, 1, rt11_name, file_seq, block_count);
    if (mt_write_rec(m->fp, eof, MT_LABEL_BYTES) != 0) goto io_err;
    if (mt_write_tape_mark(m->fp) != 0) goto io_err;
    /* Logical EOT: two more tape marks. */
    if (mt_write_tape_mark(m->fp) != 0) goto io_err;
    if (mt_write_tape_mark(m->fp) != 0) goto io_err;

    fflush(m->fp);

    end_pos = RT_FTELL64(m->fp);
    if (end_pos >= 0) {
        (void)mt_truncate(mt_fileno(m->fp), end_pos);
        m->size_bytes = end_pos;
    }

    printf("Copied '%s' -> %s (%u blocks on tape)\n",
           host_path, rt11_name, (unsigned)block_count);
    return 0;

io_err:
    fprintf(stderr, "?I/O error writing tape\n");
    if (in) fclose(in);
    return -1;
}
