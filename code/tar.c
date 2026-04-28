/*
 * tar.c - POSIX ustar archive (.tar) reader.
 *
 * Implements `tar_cmd_dir` for read-only listing of plain (un-compressed)
 * tar archives.  See tar.h for the on-disk header layout reference.
 *
 * Walking strategy:
 *
 *   pos = 0
 *   loop:
 *     read 512 B at pos
 *     if all zero -> remember; if 2 in a row -> end of archive
 *     else parse header, validate checksum, print one line, then
 *          pos += 512 + roundup(size, 512)
 *
 * Long-name extensions:
 *
 *   GNU 'L' (LongLink) and PAX 'x'/'g' write the long pathname into the
 *   data area of a synthetic record whose typeflag tells us how to
 *   interpret the next regular header.  We support GNU 'L' (most common
 *   case for files with names > 100 bytes); 'x'/'g' are flagged but the
 *   pathname falls back to whatever is in the next header's name field.
 */
#include "tar.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Header layout (offsets and sizes match the standard) --------------- */

#define TAR_OFF_NAME      0
#define TAR_LEN_NAME      100
#define TAR_OFF_MODE      100
#define TAR_LEN_MODE      8
#define TAR_OFF_SIZE      124
#define TAR_LEN_SIZE      12
#define TAR_OFF_MTIME     136
#define TAR_LEN_MTIME     12
#define TAR_OFF_CHKSUM    148
#define TAR_LEN_CHKSUM    8
#define TAR_OFF_TYPEFLAG  156
#define TAR_OFF_MAGIC     257
#define TAR_LEN_MAGIC     6
#define TAR_OFF_PREFIX    345
#define TAR_LEN_PREFIX    155

/* USTAR typeflag values we care about. */
#define TAR_TF_REGULAR    '0'
#define TAR_TF_REGULAR0   '\0'   /* historic: NUL means regular file */
#define TAR_TF_HARDLINK   '1'
#define TAR_TF_SYMLINK    '2'
#define TAR_TF_CHAR       '3'
#define TAR_TF_BLOCK      '4'
#define TAR_TF_DIR        '5'
#define TAR_TF_FIFO       '6'
#define TAR_TF_CONTIG     '7'
#define TAR_TF_GNU_LONG   'L'    /* next regular hdr name is in our data  */
#define TAR_TF_GNU_LINK   'K'    /* next regular hdr linkname in our data */
#define TAR_TF_PAX_X      'x'    /* per-file extended header              */
#define TAR_TF_PAX_G      'g'    /* global extended header                */

/* --- Helpers ---------------------------------------------------------- */

/* Read up to `len` bytes from a header field that is NOT necessarily
 * NUL-terminated, copy into `out` and zero-terminate.  `out` must be
 * len+1 bytes wide. */
static void tar_field_copy(char *out, const char *src, size_t len) {
    size_t i;
    for (i = 0; i < len && src[i]; i++) out[i] = src[i];
    out[i] = '\0';
}

/* Parse a tar octal field.  Skips leading spaces/NULs, stops at first
 * non-octal char.  Returns 0 if the field was empty or unparseable. */
static unsigned long long tar_parse_octal(const char *p, size_t len) {
    unsigned long long v = 0;
    size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\0')) i++;
    while (i < len && p[i] >= '0' && p[i] <= '7') {
        v = (v << 3) | (unsigned)(p[i] - '0');
        i++;
    }
    return v;
}

/* USTAR checksum: sum of every byte in the 512-byte header, with the
 * 8-byte chksum field treated as if it were eight ASCII spaces.  A
 * historic bug means some writers signed-sum, so accept either match. */
static int tar_checksum_ok(const unsigned char *blk) {
    unsigned long stored;
    long unsigned u_sum = 0;
    long          s_sum = 0;
    size_t i;

    stored = (unsigned long)tar_parse_octal((const char *)blk + TAR_OFF_CHKSUM,
                                            TAR_LEN_CHKSUM);
    for (i = 0; i < TAR_BLOCK_BYTES; i++) {
        unsigned char b = blk[i];
        if (i >= TAR_OFF_CHKSUM &&
            i <  TAR_OFF_CHKSUM + TAR_LEN_CHKSUM) {
            b = ' ';
        }
        u_sum += b;
        s_sum += (signed char)b;
    }
    return (u_sum == stored) || ((unsigned long)s_sum == stored);
}

/* Reconstruct full pathname into `out` (size n).  POSIX ustar splits
 * very long names as "<prefix>/<name>".  GNU long names override. */
static void tar_full_name(const unsigned char *blk,
                          const char *gnu_long,
                          char *out, size_t n) {
    char name[TAR_LEN_NAME + 1];
    char prefix[TAR_LEN_PREFIX + 1];

    if (gnu_long && gnu_long[0]) {
        /* GNU 'L' record gave us the real name; use it verbatim. */
        strlcopy(out, gnu_long, n);
        return;
    }
    tar_field_copy(name,   (const char *)blk + TAR_OFF_NAME,   TAR_LEN_NAME);
    tar_field_copy(prefix, (const char *)blk + TAR_OFF_PREFIX, TAR_LEN_PREFIX);

    /* Only POSIX ustar (magic == "ustar\0") uses the prefix field; GNU
     * tar (magic == "ustar " followed by " \0") leaves prefix empty.
     * Either way: empty prefix -> just the name. */
    if (prefix[0]) {
        snprintf(out, n, "%s/%s", prefix, name);
    } else {
        strlcopy(out, name, n);
    }
}

static const char *tar_type_short(unsigned char tf) {
    switch (tf) {
        case TAR_TF_REGULAR:
        case TAR_TF_REGULAR0:  return "    ";
        case TAR_TF_HARDLINK:  return "HLNK";
        case TAR_TF_SYMLINK:   return "SLNK";
        case TAR_TF_CHAR:      return "CHAR";
        case TAR_TF_BLOCK:     return "BLK ";
        case TAR_TF_DIR:       return "DIR ";
        case TAR_TF_FIFO:      return "FIFO";
        case TAR_TF_CONTIG:    return "CTGS";
        case TAR_TF_GNU_LONG:  return "LNAM";
        case TAR_TF_GNU_LINK:  return "LLNK";
        case TAR_TF_PAX_X:     return "PAX ";
        case TAR_TF_PAX_G:     return "PAXG";
        default:               return " ?  ";
    }
}

/* Same size/mtime formatters cmd_dir.c uses, kept private here so we
 * don't have to hoist them into a shared header.  Keep formats in sync
 * with cmd_dir.c::fmt_size / fmt_mtime if those ever change. */
static void tar_fmt_size(long long sz, char *out, size_t n) {
    if (sz < 0)              snprintf(out, n, "       -");
    else if (sz < 1024)      snprintf(out, n, "%5lld B ", sz);
    else if (sz < 1024*1024) snprintf(out, n, "%5.1f KB", sz / 1024.0);
    else                     snprintf(out, n, "%5.1f MB", sz / (1024.0 * 1024.0));
}

static void tar_fmt_mtime(time_t mt, char *out, size_t n) {
    struct tm *tm;
    if (mt == 0) { snprintf(out, n, "                "); return; }
    tm = localtime(&mt);
    if (!tm)     { snprintf(out, n, "                "); return; }
    strftime(out, n, "%Y-%m-%d %H:%M", tm);
}

/* True iff every byte of `blk` is zero (end-of-archive marker). */
static int tar_is_zero_block(const unsigned char *blk) {
    size_t i;
    for (i = 0; i < TAR_BLOCK_BYTES; i++) if (blk[i]) return 0;
    return 1;
}

/* --- Public: DIR -------------------------------------------------------- */

/* Detect well-known compressed-archive magics so we can give a useful
 * message instead of "Bad header checksum".  Returns the format name or
 * NULL if the bytes look ok. */
static const char *tar_compressed_magic(const unsigned char *p, size_t n) {
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b)              return "gzip";
    if (n >= 3 && p[0] == 'B'  && p[1] == 'Z' && p[2] == 'h') return "bzip2";
    if (n >= 6 && p[0] == 0xfd && p[1] == '7' && p[2] == 'z' &&
                  p[3] == 'X'  && p[4] == 'Z' && p[5] == 0x00) return "xz";
    if (n >= 4 && p[0] == 'P'  && p[1] == 'K' &&
                 (p[2] == 0x03 || p[2] == 0x05 || p[2] == 0x07)) return "zip";
    return NULL;
}

int tar_cmd_dir(Mount *m) {
    unsigned char blk[TAR_BLOCK_BYTES];
    char fullname[TAR_NAME_MAX];
    char gnu_long[TAR_NAME_MAX];
    char sizebuf[16];
    char timebuf[24];
    long long total_bytes = 0;
    int  count = 0;
    int  zero_run = 0;
    int  rc = 0;
    int  printed_header = 0;

    if (!m || m->kind != MOUNT_KIND_TAR) {
        fprintf(stderr, "?Not a tar archive\n");
        return -1;
    }

    if (RT_FSEEK64(m->fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "?Cannot rewind '%s'\n", m->path);
        return -1;
    }

    gnu_long[0] = '\0';

    for (;;) {
        size_t got = fread(blk, 1, TAR_BLOCK_BYTES, m->fp);
        unsigned char tf;
        unsigned long long size;
        time_t mtime;
        long long data_skip;

        if (got == 0) break;                   /* clean end of file */
        if (got != TAR_BLOCK_BYTES) {
            const char *cm = tar_compressed_magic(blk, got);
            if (cm) {
                fprintf(stderr, "?'%s' looks like a %s-compressed file, "
                        "not a plain tar.  Decompress first "
                        "(e.g. `gunzip`, `unxz`).\n", m->path, cm);
            } else {
                fprintf(stderr, "?Short read at end of archive "
                        "(got %zu of %u bytes); not a valid tar.\n",
                        got, (unsigned)TAR_BLOCK_BYTES);
            }
            rc = -1;
            break;
        }

        if (tar_is_zero_block(blk)) {
            zero_run++;
            if (zero_run >= 2) break;          /* end-of-archive marker */
            continue;
        }
        zero_run = 0;

        /* Sanity: validate header checksum.  On the very first block,
         * also sniff for compressed magics so the user gets a useful
         * hint instead of "Bad header checksum". */
        if (!tar_checksum_ok(blk)) {
            const char *cm = (count == 0)
                             ? tar_compressed_magic(blk, TAR_BLOCK_BYTES)
                             : NULL;
            if (cm) {
                fprintf(stderr, "?'%s' looks like a %s-compressed file, "
                        "not a plain tar.  Decompress first.\n",
                        m->path, cm);
            } else {
                fprintf(stderr,
                        "?Bad header checksum at offset %lld -- archive "
                        "may be corrupt or use an unsupported format.\n",
                        (long long)RT_FTELL64(m->fp) -
                        (long long)TAR_BLOCK_BYTES);
            }
            rc = -1;
            break;
        }

        /* First successful header: now print the listing header. */
        if (!printed_header) {
            printf(" Archive: %s  (POSIX ustar)\n\n", m->path);
            printf("  %-4s  %-44s %-16s %10s\n",
                   "Type", "Name", "Modified", "Size");
            printf("  ----  -------------------------------------------- "
                   "----------------  ----------\n");
            printed_header = 1;
        }

        tf    = blk[TAR_OFF_TYPEFLAG];
        size  = tar_parse_octal((const char *)blk + TAR_OFF_SIZE,  TAR_LEN_SIZE);
        mtime = (time_t)tar_parse_octal((const char *)blk + TAR_OFF_MTIME,
                                        TAR_LEN_MTIME);

        /* GNU long-name carrier: capture data into gnu_long[] and
         * suppress the placeholder line; the real entry follows. */
        if (tf == TAR_TF_GNU_LONG) {
            size_t to_read = (size_t)size;
            size_t copied  = 0;
            data_skip = (long long)((size + TAR_BLOCK_BYTES - 1) &
                                    ~(unsigned long long)(TAR_BLOCK_BYTES - 1));
            if (to_read >= sizeof(gnu_long)) {
                to_read = sizeof(gnu_long) - 1;
            }
            if (to_read > 0) {
                if (fread(gnu_long, 1, to_read, m->fp) != to_read) {
                    fprintf(stderr, "?Short read in GNU long-name record\n");
                    rc = -1;
                    break;
                }
                gnu_long[to_read] = '\0';
                copied = to_read;
            }
            /* skip remaining padding of this data area */
            if ((long long)copied < data_skip) {
                if (RT_FSEEK64(m->fp, data_skip - (long long)copied,
                               SEEK_CUR) != 0) {
                    fprintf(stderr, "?Seek error after GNU long-name\n");
                    rc = -1;
                    break;
                }
            }
            continue;   /* don't print the carrier itself */
        }

        /* Pathname (POSIX prefix+name, or GNU long override). */
        tar_full_name(blk, gnu_long, fullname, sizeof(fullname));
        gnu_long[0] = '\0';   /* consume long-name override */

        /* Directories and link-types have no data area. */
        if (tf == TAR_TF_DIR     ||
            tf == TAR_TF_HARDLINK ||
            tf == TAR_TF_SYMLINK  ||
            tf == TAR_TF_CHAR     ||
            tf == TAR_TF_BLOCK    ||
            tf == TAR_TF_FIFO) {
            data_skip = 0;
        } else {
            data_skip = (long long)((size + TAR_BLOCK_BYTES - 1) &
                                    ~(unsigned long long)(TAR_BLOCK_BYTES - 1));
        }

        if (tf == TAR_TF_DIR) {
            tar_fmt_mtime(mtime, timebuf, sizeof(timebuf));
            printf("  %s  %-44s %-16s     <DIR>\n",
                   tar_type_short(tf), fullname, timebuf);
        } else {
            tar_fmt_size((long long)size, sizebuf, sizeof(sizebuf));
            tar_fmt_mtime(mtime, timebuf, sizeof(timebuf));
            printf("  %s  %-44s %-16s  %s\n",
                   tar_type_short(tf), fullname, timebuf, sizebuf);
            total_bytes += (long long)size;
        }
        count++;

        if (data_skip > 0) {
            if (RT_FSEEK64(m->fp, data_skip, SEEK_CUR) != 0) {
                fprintf(stderr, "?Seek error skipping '%s' data\n", fullname);
                rc = -1;
                break;
            }
        }
    }

    if (printed_header) {
        printf("\n  %d entries, %lld bytes total\n", count, total_bytes);
    }
    return rc;
}

/* ----------------------------------------------------------------------
 *  COPY IN: append a host file to the archive.
 * ---------------------------------------------------------------------- */

#include <sys/stat.h>
#include <time.h>
#if defined(_MSC_VER)
#  include <io.h>
#  define tar_truncate(fd,len)  (_chsize_s((fd),(len)) == 0 ? 0 : -1)
#  define tar_fileno            _fileno
#else
#  include <unistd.h>
#  define tar_truncate(fd,len)  (ftruncate((fd),(len)) == 0 ? 0 : -1)
#  define tar_fileno            fileno
#endif

/* Format a value as a left-padded octal string of (width-1) digits + NUL.
 * Matches USTAR conventions for fields that end with NUL or space. */
static void tar_oct(char *out, int width, unsigned long long val) {
    int  i;
    out[width - 1] = '\0';
    for (i = width - 2; i >= 0; i--) {
        out[i] = (char)('0' + (val & 7));
        val >>= 3;
    }
}

/* Extract the basename from a host path. */
static const char *tar_basename(const char *path) {
    const char *p = path, *last = path;
    for (; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

/* Locate the first zero block in the archive (start of end-of-archive
 * marker, or end-of-file if archive is shorter).  Returns the byte
 * offset.  The walk parallels tar_cmd_dir but stops at the first
 * zero header. */
static long long tar_find_append_pos(FILE *fp) {
    unsigned char blk[TAR_BLOCK_BYTES];
    long long     pos = 0;
    if (RT_FSEEK64(fp, 0, SEEK_SET) != 0) return -1;
    for (;;) {
        size_t got = fread(blk, 1, TAR_BLOCK_BYTES, fp);
        if (got == 0) return pos;                   /* EOF */
        if (got != TAR_BLOCK_BYTES) return pos;     /* short read = end */
        if (tar_is_zero_block(blk)) return pos;     /* found marker */
        if (!tar_checksum_ok(blk)) {
            fprintf(stderr,
                    "?Corrupt archive (bad checksum at offset %lld); "
                    "refusing to append\n", pos);
            return -1;
        }
        /* Advance past header + data area. */
        {
            unsigned long long size =
                tar_parse_octal((const char *)blk + TAR_OFF_SIZE,
                                TAR_LEN_SIZE);
            unsigned char tf = blk[TAR_OFF_TYPEFLAG];
            long long data_skip;
            if (tf == TAR_TF_DIR  || tf == TAR_TF_HARDLINK ||
                tf == TAR_TF_SYMLINK || tf == TAR_TF_CHAR ||
                tf == TAR_TF_BLOCK   || tf == TAR_TF_FIFO) {
                data_skip = 0;
            } else {
                data_skip = (long long)((size + TAR_BLOCK_BYTES - 1) &
                                        ~(unsigned long long)(TAR_BLOCK_BYTES - 1));
            }
            pos += TAR_BLOCK_BYTES + data_skip;
            if (data_skip > 0 &&
                RT_FSEEK64(fp, data_skip, SEEK_CUR) != 0) {
                return -1;
            }
        }
    }
}

int tar_copy_in(Mount *m, const char *host_path, const char *tape_name) {
    FILE          *in;
    long long      append_at;
    long long      file_size;
    long long      written = 0;
    unsigned char  hdr[TAR_BLOCK_BYTES];
    unsigned char  zero[TAR_BLOCK_BYTES];
    unsigned char  buf[TAR_BLOCK_BYTES];
    const char    *name;
    struct stat    st;
    unsigned long  csum = 0;
    size_t         i;
    char           csum_buf[8];

    if (!m || !m->fp || !host_path) return -1;
    if (m->kind != MOUNT_KIND_TAR) {
        fprintf(stderr, "?'%s' is not a .tar archive\n", m->path);
        return -1;
    }

    /* Open host file and stat it. */
    if (stat(host_path, &st) != 0) {
        fprintf(stderr, "?Cannot stat host file '%s'\n", host_path);
        return -1;
    }
    in = fopen(host_path, "rb");
    if (!in) {
        fprintf(stderr, "?Cannot open '%s' for reading\n", host_path);
        return -1;
    }
    file_size = (long long)st.st_size;

    /* Decide the archive entry name. */
    name = (tape_name && *tape_name) ? tape_name : tar_basename(host_path);
    if (strlen(name) >= TAR_LEN_NAME) {
        fprintf(stderr,
                "?Archive name '%s' too long for ustar (max 99 chars; "
                "GNU long-name not implemented for write)\n", name);
        fclose(in);
        return -1;
    }

    /* Find where to append. */
    append_at = tar_find_append_pos(m->fp);
    if (append_at < 0) {
        fclose(in);
        return -1;
    }

    /* Build the USTAR header. */
    memset(hdr, 0, sizeof(hdr));
    strlcopy((char *)hdr + TAR_OFF_NAME, name, TAR_LEN_NAME);
    tar_oct((char *)hdr + 100, 8, 0644u);                 /* mode */
    tar_oct((char *)hdr + 108, 8, 0u);                    /* uid  */
    tar_oct((char *)hdr + 116, 8, 0u);                    /* gid  */
    tar_oct((char *)hdr + TAR_OFF_SIZE,  TAR_LEN_SIZE,  (unsigned long long)file_size);
    tar_oct((char *)hdr + TAR_OFF_MTIME, TAR_LEN_MTIME, (unsigned long long)st.st_mtime);
    /* Checksum field: 8 spaces while computing, then 6 octal + NUL + space. */
    memset(hdr + TAR_OFF_CHKSUM, ' ', TAR_LEN_CHKSUM);
    hdr[TAR_OFF_TYPEFLAG] = TAR_TF_REGULAR;
    /* magic + version: POSIX ustar = "ustar\0" + "00" */
    memcpy(hdr + TAR_OFF_MAGIC, "ustar", 5);              /* +5 = NUL */
    hdr[263] = '0';
    hdr[264] = '0';

    /* Compute checksum: simple sum of all 512 bytes (with chksum
     * field treated as spaces, which we already have). */
    for (i = 0; i < TAR_BLOCK_BYTES; i++) csum += hdr[i];
    /* Write 6 octal digits + NUL + space at chksum field. */
    {
        char *cs = (char *)hdr + TAR_OFF_CHKSUM;
        unsigned long v = csum;
        int j;
        for (j = 5; j >= 0; j--) { cs[j] = (char)('0' + (v & 7)); v >>= 3; }
        cs[6] = '\0';
        cs[7] = ' ';
    }
    (void)csum_buf;

    /* Seek to append point and write header. */
    if (RT_FSEEK64(m->fp, append_at, SEEK_SET) != 0) {
        fprintf(stderr, "?Seek to %lld failed\n", append_at);
        fclose(in); return -1;
    }
    if (fwrite(hdr, 1, TAR_BLOCK_BYTES, m->fp) != TAR_BLOCK_BYTES) {
        fprintf(stderr, "?Write header failed\n");
        fclose(in); return -1;
    }

    /* Stream file data, padding the last block to 512 with zeros. */
    while (written < file_size) {
        size_t toread = TAR_BLOCK_BYTES;
        size_t got;
        if ((long long)(written + (long long)toread) > file_size) {
            toread = (size_t)(file_size - written);
        }
        memset(buf, 0, sizeof(buf));
        got = fread(buf, 1, toread, in);
        if (got != toread) {
            fprintf(stderr, "?Read error on '%s' at offset %lld\n",
                    host_path, written);
            fclose(in); return -1;
        }
        if (fwrite(buf, 1, TAR_BLOCK_BYTES, m->fp) != TAR_BLOCK_BYTES) {
            fprintf(stderr, "?Write data failed\n");
            fclose(in); return -1;
        }
        written += (long long)got;
    }
    fclose(in);

    /* Trailing two zero blocks (end-of-archive). */
    memset(zero, 0, sizeof(zero));
    if (fwrite(zero, 1, TAR_BLOCK_BYTES, m->fp) != TAR_BLOCK_BYTES ||
        fwrite(zero, 1, TAR_BLOCK_BYTES, m->fp) != TAR_BLOCK_BYTES) {
        fprintf(stderr, "?Write end-of-archive failed\n");
        return -1;
    }
    /* Truncate at end of our two zero blocks so any pre-existing tail
     * (from a previous longer archive) is dropped. */
    fflush(m->fp);
    {
        long long want = append_at + TAR_BLOCK_BYTES +
                         ((file_size + TAR_BLOCK_BYTES - 1) &
                          ~(long long)(TAR_BLOCK_BYTES - 1)) +
                         2 * (long long)TAR_BLOCK_BYTES;
        (void)tar_truncate(tar_fileno(m->fp), want);
    }

    printf("Copied '%s' -> %s:%s  (%lld bytes, %lld blocks)\n",
           host_path, m->path, name, file_size,
           (file_size + TAR_BLOCK_BYTES - 1) / TAR_BLOCK_BYTES);
    return 0;
}
