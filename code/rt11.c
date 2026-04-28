/*
 * rt11.c - RT-11 / TSX+ filesystem operations on a DV image file.
 */
#include "rt11.h"
#include "rad50.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 *   Block-level I/O
 * ====================================================================== */

static int seek_block(Rt11Dev *d, uint32_t blk) {
    long long off = (long long)blk * (long long)RT11_BLOCK_SIZE;
    if (RT_FSEEK64(d->fp, off, SEEK_SET) != 0) {
        fprintf(stderr, "?Seek error (blk=%u): %s\n", blk, strerror(errno));
        return -1;
    }
    return 0;
}

int rt11_read_block(Rt11Dev *d, uint32_t blk, void *buf) {
    return rt11_read_blocks(d, blk, 1, buf);
}

int rt11_write_block(Rt11Dev *d, uint32_t blk, const void *buf) {
    return rt11_write_blocks(d, blk, 1, buf);
}

int rt11_read_blocks(Rt11Dev *d, uint32_t blk, uint32_t n, void *buf) {
    size_t want;
    size_t got;
    uint32_t total = d->total_blocks ? d->total_blocks : RT11_DV_BLOCKS;

    if (blk + n > total) {
        fprintf(stderr, "?Read past end of DV (blk=%u cnt=%u total=%u)\n",
                blk, n, total);
        return -1;
    }
    if (seek_block(d, blk) != 0) return -1;
    want = (size_t)n * RT11_BLOCK_SIZE;
    got  = fread(buf, 1, want, d->fp);
    if (got != want) {
        fprintf(stderr, "?Short read (blk=%u want=%zu got=%zu)\n",
                blk, want, got);
        return -1;
    }
    return 0;
}

int rt11_write_blocks(Rt11Dev *d, uint32_t blk, uint32_t n, const void *buf) {
    size_t want;
    size_t got;
    uint32_t total = d->total_blocks ? d->total_blocks : RT11_DV_BLOCKS;

    if (blk + n > total) {
        fprintf(stderr, "?Write past end of DV (blk=%u cnt=%u total=%u)\n",
                blk, n, total);
        return -1;
    }
    if (seek_block(d, blk) != 0) return -1;
    want = (size_t)n * RT11_BLOCK_SIZE;
    got  = fwrite(buf, 1, want, d->fp);
    if (got != want) {
        fprintf(stderr, "?Short write (blk=%u want=%zu got=%zu)\n",
                blk, want, got);
        return -1;
    }
    fflush(d->fp);
    return 0;
}

/* ======================================================================
 *   CREATE   -   make an empty 10 MB file filled with zeros
 * ====================================================================== */

int rt11_create_image(const char *path) {
    FILE *fp;
    uint8_t zero[RT11_BLOCK_SIZE];
    uint32_t i;

    /* Refuse to clobber an existing file - caller can delete it first. */
    fp = fopen(path, "rb");
    if (fp) {
        fclose(fp);
        fprintf(stderr, "?File '%s' already exists\n", path);
        return -1;
    }

    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "?Cannot create '%s': %s\n", path, strerror(errno));
        return -1;
    }

    memset(zero, 0, sizeof(zero));
    for (i = 0; i < RT11_DV_BLOCKS; i++) {
        if (fwrite(zero, 1, RT11_BLOCK_SIZE, fp) != RT11_BLOCK_SIZE) {
            fprintf(stderr, "?Write error while creating '%s'\n", path);
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

/* ======================================================================
 *   FORMAT   -   write home block + empty directory
 * ====================================================================== */

/* Compute the 16-bit sum-mod-65536 of the first 255 words of the buffer
 * and store it as the 256th word (offset 0776 octal). */
static void home_checksum(uint8_t *blk) {
    uint32_t sum = 0;
    size_t i;
    for (i = 0; i < 255; i++) {
        sum += rd_u16(blk, i * 2);
    }
    wr_u16(blk, HB_CHECKSUM_OFF, (uint16_t)(sum & 0xFFFF));
}

static void put_ascii12(uint8_t *blk, size_t off, const char *s) {
    size_t i;
    for (i = 0; i < 12; i++) {
        char c;
        if (s && s[i]) {
            c = (char)toupper((unsigned char)s[i]);
        } else {
            c = ' ';
        }
        blk[off + i] = (uint8_t)c;
    }
}

int rt11_format(Rt11Dev *d, const char *vol_id, const char *owner_name) {
    uint8_t blk[RT11_BLOCK_SIZE];
    uint8_t seg[RT11_SEG_BYTES];
    uint32_t first_data;
    uint32_t s;

    /* --- Home block ---------------------------------------------------- */
    memset(blk, 0, sizeof(blk));
    wr_u16(blk, HB_CLUSTER_OFF, 1);                     /* cluster size */
    wr_u16(blk, HB_DIR_BLK_OFF, RT11_DIR_START_BLOCK);  /* dir @ blk 6  */
    wr_u16(blk, HB_SYSVER_OFF,  rad50_encode3("V05"));  /* V05 in RAD50 */
    put_ascii12(blk, HB_VOLID_OFF,  vol_id      ? vol_id      : "RT11A       ");
    put_ascii12(blk, HB_OWNER_OFF,  owner_name  ? owner_name  : "            ");
    put_ascii12(blk, HB_SYSID_OFF,  "DECRT11A    ");
    home_checksum(blk);

    if (rt11_write_block(d, RT11_HOME_BLOCK, blk) != 0) return -1;

    /* --- Zero the boot area (blk 0 and blks 2..5) ---------------------- */
    memset(blk, 0, sizeof(blk));
    if (rt11_write_block (d, 0, blk) != 0) return -1;
    if (rt11_write_blocks(d, 2, 4, blk) != 0) {
        uint32_t b;
        for (b = 2; b < 6; b++) {
            if (rt11_write_block(d, b, blk) != 0) return -1;
        }
    }

    /* --- Directory segments ------------------------------------------- */
    first_data = RT11_DIR_START_BLOCK +
                 (RT11_MAX_SEGMENTS * RT11_SEG_BLOCKS);   /* = 68 */

    for (s = 1; s <= RT11_MAX_SEGMENTS; s++) {
        memset(seg, 0, sizeof(seg));

        if (s == 1) {
            /* 5-word header */
            wr_u16(seg,  0, (uint16_t)RT11_MAX_SEGMENTS);  /* total segs  */
            wr_u16(seg,  2, 0);                            /* next seg    */
            wr_u16(seg,  4, 1);                            /* highest in use */
            wr_u16(seg,  6, 0);                            /* extra bytes */
            wr_u16(seg,  8, (uint16_t)first_data);         /* 1st file blk*/

            /* Entry 0: < UNUSED > covering the entire data area. */
            {
                uint32_t total = d->total_blocks
                                 ? d->total_blocks : RT11_DV_BLOCKS;
                wr_u16(seg, 10, RT11_E_UNUSED);            /* status      */
                wr_u16(seg, 12, 0);                        /* FN word 1   */
                wr_u16(seg, 14, 0);                        /* FN word 2   */
                wr_u16(seg, 16, 0);                        /* EXT         */
                wr_u16(seg, 18, (uint16_t)(total - first_data));
            }
            wr_u16(seg, 20, 0);                            /* chan/job    */
            wr_u16(seg, 22, 0);                            /* date        */

            /* Entry 1: end-of-segment marker */
            wr_u16(seg, 24, RT11_E_ENDSEG);
        } else {
            /* Reserved segment - fully zero so next_seg lookup stops
             * at seg 1. It's OK to leave this zeroed. */
        }

        if (rt11_write_blocks(d,
                              RT11_DIR_START_BLOCK + (s - 1) * RT11_SEG_BLOCKS,
                              RT11_SEG_BLOCKS, seg) != 0) {
            return -1;
        }
    }

    return 0;
}

/* ======================================================================
 *   "Is the volume formatted?" check
 *
 *   Used at the top of every command that reads the directory.  It is
 *   deliberately conservative: we want to reject zero-filled / never-
 *   formatted disks with a clear message instead of crashing inside the
 *   segment walker ("Segment overflow while loading").
 * ====================================================================== */

int rt11_is_formatted(Rt11Dev *d) {
    uint8_t home[RT11_BLOCK_SIZE];
    uint8_t seg[RT11_SEG_BYTES];
    uint16_t dir_blk, total_segs, extra, first_file;
    uint32_t total = d->total_blocks ? d->total_blocks : RT11_DV_BLOCKS;
    char sysid[13];

    if (rt11_read_block(d, RT11_HOME_BLOCK, home) != 0) return 0;

    /* System ID should be "DECRT11A" in the first 8 bytes of the sysid
     * field (the remaining 4 bytes are spaces). */
    memcpy(sysid, home + HB_SYSID_OFF, 12);
    sysid[12] = 0;
    if (memcmp(sysid, "DECRT11A", 8) != 0) return 0;

    dir_blk = rd_u16(home, HB_DIR_BLK_OFF);
    if (dir_blk != RT11_DIR_START_BLOCK) return 0;

    /* Segment 1 must have a sensible header.  Real RT-11 volumes can have
     * any number of segments from 1 to 31 (a 5 MB disk uses 16, a 10 MB
     * disk typically uses 31) and the data area starts right after the
     * reserved segments - so the first-file-block word varies too. */
    if (rt11_read_blocks(d, RT11_DIR_START_BLOCK, RT11_SEG_BLOCKS, seg) != 0)
        return 0;
    total_segs = rd_u16(seg, 0);
    extra      = rd_u16(seg, 6);
    first_file = rd_u16(seg, 8);
    if (total_segs < 1 || total_segs > RT11_MAX_SEGMENTS) return 0;
    if (extra > 100)                                      return 0;
    if (first_file < RT11_DIR_START_BLOCK + total_segs * RT11_SEG_BLOCKS)
        return 0;
    if (first_file >= total)                              return 0;
    return 1;
}

/* ======================================================================
 *   Directory iteration
 * ====================================================================== */

static int read_segment(Rt11Dev *d, uint32_t seg_no, uint8_t *buf) {
    uint32_t blk;
    if (seg_no < 1 || seg_no > RT11_MAX_SEGMENTS) {
        fprintf(stderr, "?Bad segment number %u\n", seg_no);
        return -1;
    }
    blk = RT11_DIR_START_BLOCK + (seg_no - 1) * RT11_SEG_BLOCKS;
    return rt11_read_blocks(d, blk, RT11_SEG_BLOCKS, buf);
}

static int write_segment(Rt11Dev *d, uint32_t seg_no, const uint8_t *buf) {
    uint32_t blk;
    if (seg_no < 1 || seg_no > RT11_MAX_SEGMENTS) {
        fprintf(stderr, "?Bad segment number %u\n", seg_no);
        return -1;
    }
    blk = RT11_DIR_START_BLOCK + (seg_no - 1) * RT11_SEG_BLOCKS;
    return rt11_write_blocks(d, blk, RT11_SEG_BLOCKS, buf);
}

int rt11_dir_begin(Rt11Dev *d, Rt11DirIter *it) {
    memset(it, 0, sizeof(*it));
    it->dev    = d;
    it->seg_no = 1;
    if (read_segment(d, 1, it->segbuf) != 0) return -1;
    it->total_segs  = rd_u16(it->segbuf, 0);
    it->extra_bytes = rd_u16(it->segbuf, 6);
    it->cur_block   = rd_u16(it->segbuf, 8);
    it->cur_offset  = (uint32_t)(RT11_SEG_HEADER_WORDS * 2);
    return 0;
}

int rt11_dir_next(Rt11DirIter *it, Rt11DirEntry *e) {
    uint32_t esz;
    uint16_t next_seg;
    uint16_t chanjob;

    esz = RT11_ENTRY_BASE_BYTES + it->extra_bytes;

    for (;;) {
        if (it->cur_offset + 2 > RT11_SEG_BYTES) {
            /* Ran off the end without seeing END_SEG - try next segment. */
            next_seg = rd_u16(it->segbuf, 2);
            if (next_seg == 0) return 0;
            it->seg_no = next_seg;
            if (read_segment(it->dev, it->seg_no, it->segbuf) != 0) return -1;
            it->extra_bytes = rd_u16(it->segbuf, 6);
            it->cur_block   = rd_u16(it->segbuf, 8);
            it->cur_offset  = (uint32_t)(RT11_SEG_HEADER_WORDS * 2);
            esz = RT11_ENTRY_BASE_BYTES + it->extra_bytes;
            continue;
        }

        e->status = rd_u16(it->segbuf, it->cur_offset);
        if (e->status == 0 ||
            (e->status & (RT11_E_TENTATIVE | RT11_E_UNUSED |
                          RT11_E_PERMANENT | RT11_E_ENDSEG)) == 0) {
            fprintf(stderr, "?Corrupt directory entry "
                            "(seg=%u off=%u status=0%o)\n",
                    it->seg_no, it->cur_offset, e->status);
            return -1;
        }
        if (e->status & RT11_E_ENDSEG) {
            /* End of this segment: jump to next if there is one. */
            next_seg = rd_u16(it->segbuf, 2);
            if (next_seg == 0) return 0;
            it->seg_no = next_seg;
            if (read_segment(it->dev, it->seg_no, it->segbuf) != 0) return -1;
            it->extra_bytes = rd_u16(it->segbuf, 6);
            it->cur_block   = rd_u16(it->segbuf, 8);
            it->cur_offset  = (uint32_t)(RT11_SEG_HEADER_WORDS * 2);
            esz = RT11_ENTRY_BASE_BYTES + it->extra_bytes;
            continue;
        }

        /* Real entry (PERMANENT, UNUSED or TENTATIVE). */
        e->fn_hi       = rd_u16(it->segbuf, it->cur_offset + 2);
        e->fn_lo       = rd_u16(it->segbuf, it->cur_offset + 4);
        e->ext         = rd_u16(it->segbuf, it->cur_offset + 6);
        e->length      = rd_u16(it->segbuf, it->cur_offset + 8);
        chanjob        = rd_u16(it->segbuf, it->cur_offset + 10);
        e->channel     = (uint8_t)(chanjob & 0xFF);
        e->job         = (uint8_t)((chanjob >> 8) & 0xFF);
        e->date        = rd_u16(it->segbuf, it->cur_offset + 12);
        e->start_block = it->cur_block;
        e->seg_no      = it->seg_no;
        e->seg_offset  = it->cur_offset;

        it->cur_block  += e->length;
        it->cur_offset += esz;
        return 1;
    }
}

/* ======================================================================
 *   DIR command
 * ====================================================================== */

int rt11_cmd_dir(Rt11Dev *d) {
    uint8_t home[RT11_BLOCK_SIZE];
    Rt11DirIter it;
    Rt11DirEntry e;
    int rc;
    int nfiles = 0, nblocks = 0, nfree = 0;
    char fn[12];
    char volid[13];
    char ownr[13];
    int col;
    rt11_datefmt_t datefmt;

    if (!rt11_is_formatted(d)) {
        fprintf(stderr, "?Volume is not formatted as RT-11 "
                        "(use FORMAT first)\n");
        return -1;
    }

    /* Print volume info. */
    if (rt11_read_block(d, RT11_HOME_BLOCK, home) != 0) return -1;
    memcpy(volid, home + HB_VOLID_OFF, 12); volid[12] = 0;
    memcpy(ownr,  home + HB_OWNER_OFF, 12); ownr[12]  = 0;
    datefmt = rt11_datefmt_from_sysver(rd_u16(home, HB_SYSVER_OFF));
    printf(" Volume ID: %s\n", volid);
    printf(" Owner    : %s\n", ownr);
    printf(" Dates    : %s layout\n", rt11_datefmt_name(datefmt));
    printf("\n");

    if (rt11_dir_begin(d, &it) != 0) return -1;

    col = 0;
    for (;;) {
        rc = rt11_dir_next(&it, &e);
        if (rc < 0) return -1;
        if (rc == 0) break;

        if (e.status & RT11_E_UNUSED) {
            printf(" %-10s %5u", "< UNUSED >", (unsigned)e.length);
            nfree += e.length;
        } else if (e.status & RT11_E_PERMANENT) {
            int y, m, dy;
            rad50_decode_filename(e.fn_hi, e.fn_lo, e.ext, fn);
            printf(" %-10s %5u", fn, (unsigned)e.length);
            if (e.status & RT11_E_PROTECTED) {
                printf("P");
            } else {
                printf(" ");
            }
            if (rt11_decode_date(e.date, datefmt, &y, &m, &dy) == 0) {
                static const char *mon[] = {
                    "Jan","Feb","Mar","Apr","May","Jun",
                    "Jul","Aug","Sep","Oct","Nov","Dec"
                };
                const char *mstr = (m >= 1 && m <= 12) ? mon[m - 1] : "???";
                printf(" %02d-%s-%04d", dy, mstr, y);
            } else {
                printf("           ");
            }
            nfiles++;
            nblocks += e.length;
        } else {
            /* tentative or other */
            rad50_decode_filename(e.fn_hi, e.fn_lo, e.ext, fn);
            printf(" %-10s %5u (tentative)", fn, (unsigned)e.length);
        }
        col++;
        if (col >= 2) {
            printf("\n");
            col = 0;
        } else {
            printf("    ");
        }
    }
    if (col) printf("\n");

    printf("\n %d Files, %d Blocks\n", nfiles, nblocks);
    printf(" %d Free blocks\n", nfree);
    return 0;
}

/* ======================================================================
 *   Internal: load the entire directory into a linear in-memory image
 *   (5-word header + concatenated real entries, end-seg markers dropped).
 *
 *   On success:
 *       *out_buf = heap buffer, caller must free().
 *       *out_len = number of bytes in the logical directory (incl hdr).
 *       *out_entry_bytes = 14 + extra_bytes.
 *       *out_total_segs  = total segments available (word 0 of seg 1).
 * ====================================================================== */

static int load_dir_linear(Rt11Dev *d,
                           uint8_t  **out_buf,
                           size_t    *out_len,
                           uint32_t  *out_entry_bytes,
                           uint16_t  *out_total_segs,
                           uint16_t  *out_first_file_blk) {
    uint8_t seg[RT11_SEG_BYTES];
    uint8_t *big = NULL;
    size_t   cap = 0;
    size_t   len = 0;
    uint16_t next_seg = 1;
    uint16_t extra = 0;
    uint32_t esz;
    uint16_t total_segs = 0;
    uint16_t first_file_blk = 0;
    int first = 1;

    while (next_seg != 0) {
        uint32_t off;
        if (next_seg > RT11_MAX_SEGMENTS) {
            fprintf(stderr, "?Corrupt directory: seg=%u\n", next_seg);
            goto fail;
        }
        if (read_segment(d, next_seg, seg) != 0) goto fail;

        if (first) {
            total_segs     = rd_u16(seg, 0);
            extra          = rd_u16(seg, 6);
            first_file_blk = rd_u16(seg, 8);
            /* copy the 5-word header verbatim */
            cap = RT11_SEG_BYTES * (size_t)RT11_MAX_SEGMENTS;
            big = (uint8_t *)calloc(1, cap);
            if (!big) { fprintf(stderr, "?Out of memory\n"); goto fail; }
            memcpy(big, seg, 10);
            len = 10;
            first = 0;
        }

        esz = RT11_ENTRY_BASE_BYTES + extra;
        off = RT11_SEG_HEADER_WORDS * 2;
        while (off + 2 <= RT11_SEG_BYTES) {
            uint16_t st = rd_u16(seg, off);
            if (st & RT11_E_ENDSEG) break;
            if (st == 0 ||
                (st & (RT11_E_TENTATIVE | RT11_E_UNUSED |
                       RT11_E_PERMANENT)) == 0) {
                fprintf(stderr, "?Corrupt directory entry "
                                "(seg=%u off=%u status=0%o)\n",
                        (unsigned)next_seg, off, st);
                goto fail;
            }
            if (off + esz > RT11_SEG_BYTES) {
                fprintf(stderr, "?Segment overflow while loading\n");
                goto fail;
            }
            if (len + esz > cap) {
                fprintf(stderr, "?Directory larger than buffer\n");
                goto fail;
            }
            memcpy(big + len, seg + off, esz);
            len += esz;
            off += esz;
        }
        next_seg = rd_u16(seg, 2);
    }

    *out_buf            = big;
    *out_len            = len;
    *out_entry_bytes    = RT11_ENTRY_BASE_BYTES + extra;
    *out_total_segs     = total_segs;
    *out_first_file_blk = first_file_blk;
    return 0;

fail:
    free(big);
    return -1;
}

/* Rewrite the entire directory from a linear in-memory image.
 *
 * `total_segs` is the number of directory segments the volume was
 * formatted with (read from word 0 of segment 1).  Only those segments
 * belong to the directory area; segments past `total_segs` would sit in
 * the data area on a small volume, so we must NOT touch them.
 * `first_file_blk` is the volume's first data block (segment 1 word 4). */
static int store_dir_linear(Rt11Dev *d,
                            const uint8_t *big,
                            size_t big_len,
                            uint32_t entry_bytes,
                            uint16_t total_segs,
                            uint16_t first_file_blk) {
    uint8_t seg[RT11_SEG_BYTES];
    uint32_t s;
    uint32_t entries_per_seg;
    uint32_t entries_total;
    uint32_t needed_segs;
    uint32_t ent_idx;
    uint32_t header_first_file;
    uint32_t running_block;
    uint32_t src_off;

    entries_per_seg = (RT11_SEG_BYTES -
                       (RT11_SEG_HEADER_WORDS * 2) - 2) / entry_bytes;
    entries_total   = (uint32_t)((big_len - 10) / entry_bytes);
    needed_segs     = (entries_total + entries_per_seg - 1) / entries_per_seg;
    if (needed_segs == 0) needed_segs = 1;
    if (needed_segs > total_segs) {
        fprintf(stderr, "?Directory full (need %u segs, have %u)\n",
                needed_segs, total_segs);
        return -1;
    }

    running_block = first_file_blk;
    ent_idx = 0;
    src_off = 10; /* skip source header */

    for (s = 1; s <= total_segs; s++) {
        memset(seg, 0, sizeof(seg));

        if (s <= needed_segs) {
            uint32_t out_off = RT11_SEG_HEADER_WORDS * 2;
            uint32_t remaining = entries_total - ent_idx;
            uint32_t take = remaining < entries_per_seg
                            ? remaining
                            : entries_per_seg;
            uint32_t i;

            header_first_file = running_block;

            /* header */
            wr_u16(seg,  0, (uint16_t)total_segs);
            wr_u16(seg,  2, (s < needed_segs) ? (uint16_t)(s + 1) : 0);
            wr_u16(seg,  4, (uint16_t)needed_segs);
            wr_u16(seg,  6, (uint16_t)(entry_bytes - RT11_ENTRY_BASE_BYTES));
            wr_u16(seg,  8, (uint16_t)header_first_file);

            for (i = 0; i < take; i++) {
                memcpy(seg + out_off, big + src_off, entry_bytes);
                running_block += rd_u16(seg, out_off + 8);
                out_off += entry_bytes;
                src_off += entry_bytes;
                ent_idx++;
            }
            /* end-of-seg marker at out_off */
            wr_u16(seg, out_off, RT11_E_ENDSEG);
        }
        /* else: leave this directory segment zeroed (still reserved for
         * future growth within the already-allocated directory area). */

        if (write_segment(d, s, seg) != 0) return -1;
    }
    return 0;
}

/* ======================================================================
 *   COPY   -   host file -> DV
 * ====================================================================== */

/* Build a filename in 8.3 form from a host path (base name only, uppercase,
 * non-RAD50 chars stripped). Returns 0 on success, fills dv_name (min 12). */
static int derive_dv_name(const char *host_path, char *dv_name, size_t sz) {
    const char *p;
    const char *slash;
    const char *dot;
    size_t nlen, elen, i;
    char base[16];
    char ext[8];

    p = host_path;
    slash = strrchr(p, '\\');
    if (!slash) slash = strrchr(p, '/');
    if (slash) p = slash + 1;

    dot = strrchr(p, '.');
    if (dot) {
        nlen = (size_t)(dot - p);
        elen = strlen(dot + 1);
    } else {
        nlen = strlen(p);
        elen = 0;
    }
    if (nlen > 6) nlen = 6;
    if (elen > 3) elen = 3;
    if (nlen == 0) return -1;

    for (i = 0; i < nlen; i++) base[i] = p[i];
    base[nlen] = 0;
    for (i = 0; i < elen; i++) ext[i] = dot[1 + i];
    ext[elen] = 0;
    strupper(base);
    strupper(ext);

    if (elen > 0) {
        snprintf(dv_name, sz, "%s.%s", base, ext);
    } else {
        strlcopy(dv_name, base, sz);
    }
    return 0;
}

int rt11_copy_in(Rt11Dev *d, const char *host_path, const char *dv_name_opt) {
    FILE *fp = NULL;
    long long fsize;
    uint32_t nblocks;
    uint8_t  *dir = NULL;
    size_t    dir_len = 0;
    uint32_t  entry_bytes = 0;
    uint16_t  total_segs = 0;
    uint16_t  first_file_blk = 0;
    uint16_t  fn[3];
    char      dv_name[16];
    uint32_t  data_off;
    uint32_t  running_block;
    uint32_t  found_off = 0;
    uint32_t  found_size = 0;
    int       found = 0;
    uint32_t  first_data;
    int       y, m, dy;
    uint16_t  date_w;
    uint8_t  *buf = NULL;
    uint32_t  blk_to_write;
    uint32_t  new_dir_len;
    uint32_t  prev_size;
    uint8_t  *new_dir = NULL;

    if (!rt11_is_formatted(d)) {
        fprintf(stderr, "?Volume is not formatted as RT-11 "
                        "(use FORMAT first)\n");
        return -1;
    }

    /* Open host file. */
    fp = fopen(host_path, "rb");
    if (!fp) {
        fprintf(stderr, "?Cannot open '%s': %s\n", host_path, strerror(errno));
        return -1;
    }
    if (RT_FSEEK64(fp, 0, SEEK_END) != 0) goto ioerr;
    fsize = RT_FTELL64(fp);
    if (fsize < 0) goto ioerr;
    if (RT_FSEEK64(fp, 0, SEEK_SET) != 0) goto ioerr;

    nblocks = (uint32_t)((fsize + RT11_BLOCK_SIZE - 1) / RT11_BLOCK_SIZE);
    if (nblocks == 0) nblocks = 1;   /* at least one block */

    /* Build RT-11 filename. */
    if (dv_name_opt && *dv_name_opt) {
        strlcopy(dv_name, dv_name_opt, sizeof(dv_name));
        strupper(dv_name);
    } else {
        if (derive_dv_name(host_path, dv_name, sizeof(dv_name)) != 0) {
            fprintf(stderr, "?Cannot derive RT-11 name from '%s'\n", host_path);
            fclose(fp);
            return -1;
        }
    }
    if (rad50_encode_filename(dv_name, fn) != 0) {
        fprintf(stderr, "?Invalid RT-11 filename '%s'\n", dv_name);
        fclose(fp);
        return -1;
    }

    /* Read the entire directory. */
    if (load_dir_linear(d, &dir, &dir_len, &entry_bytes, &total_segs,
                        &first_file_blk) != 0) {
        fclose(fp);
        return -1;
    }

    first_data = first_file_blk;

    /* First pass: delete any existing PERMANENT file with the same name. */
    data_off = 10;
    while (data_off < dir_len) {
        uint16_t st = rd_u16(dir, data_off);
        if ((st & RT11_E_PERMANENT) != 0) {
            if (rd_u16(dir, data_off + 2) == fn[0] &&
                rd_u16(dir, data_off + 4) == fn[1] &&
                rd_u16(dir, data_off + 6) == fn[2]) {
                if (st & RT11_E_PROTECTED) {
                    fprintf(stderr, "?Cannot overwrite protected file %s\n",
                            dv_name);
                    goto fail;
                }
                /* mark as UNUSED */
                wr_u16(dir, data_off, RT11_E_UNUSED);
                wr_u16(dir, data_off + 2, 0);
                wr_u16(dir, data_off + 4, 0);
                wr_u16(dir, data_off + 6, 0);
                /* keep length, clear chan/job/date */
                wr_u16(dir, data_off + 10, 0);
                wr_u16(dir, data_off + 12, 0);
            }
        }
        data_off += entry_bytes;
    }

    /* Second pass: merge adjacent UNUSED runs (so a fresh copy can use the
     * whole freed area). */
    {
        uint32_t off = 10;
        while (off + entry_bytes < dir_len) {
            uint16_t st  = rd_u16(dir, off);
            uint16_t st2 = rd_u16(dir, off + entry_bytes);
            if ((st & RT11_E_UNUSED) && (st2 & RT11_E_UNUSED)) {
                uint32_t combined = rd_u16(dir, off + 8) +
                                    rd_u16(dir, off + entry_bytes + 8);
                if (combined > 0xFFFF) { off += entry_bytes; continue; }
                wr_u16(dir, off + 8, (uint16_t)combined);
                memmove(dir + off + entry_bytes,
                        dir + off + entry_bytes * 2,
                        (dir_len - (off + entry_bytes * 2)));
                dir_len -= entry_bytes;
                /* don't advance - maybe more UNUSED follows */
            } else {
                off += entry_bytes;
            }
        }
    }

    /* Third pass: find first-fit UNUSED >= nblocks, tracking start block. */
    running_block = first_data;
    data_off = 10;
    while (data_off < dir_len) {
        uint16_t st = rd_u16(dir, data_off);
        uint16_t sz = rd_u16(dir, data_off + 8);
        if ((st & RT11_E_UNUSED) && sz >= nblocks) {
            found = 1;
            found_off  = data_off;
            found_size = sz;
            break;
        }
        running_block += sz;
        data_off += entry_bytes;
    }
    if (!found) {
        fprintf(stderr, "?No UNUSED area large enough for %u blocks\n",
                nblocks);
        goto fail;
    }

    /* Write the data. */
    buf = (uint8_t *)malloc(RT11_BLOCK_SIZE);
    if (!buf) { fprintf(stderr, "?Out of memory\n"); goto fail; }
    blk_to_write = running_block;
    {
        uint32_t i;
        for (i = 0; i < nblocks; i++) {
            size_t n;
            memset(buf, 0, RT11_BLOCK_SIZE);
            n = fread(buf, 1, RT11_BLOCK_SIZE, fp);
            if (n == 0 && ferror(fp)) {
                fprintf(stderr, "?Read error on '%s'\n", host_path);
                goto fail;
            }
            /* if last block is partial, buf already zero-padded */
            if (rt11_write_block(d, blk_to_write + i, buf) != 0) goto fail;
        }
    }
    free(buf); buf = NULL;
    fclose(fp); fp = NULL;

    /* Update the directory in memory.  Match the date-word layout to
     * the volume's SYSVER so that older RT-11 V3/V4 monitors can still
     * read the dates we write back. */
    today_ymd(&y, &m, &dy);
    {
        uint8_t hb[RT11_BLOCK_SIZE];
        rt11_datefmt_t fmt = RT11_DATEFMT_V55;
        if (rt11_read_block(d, RT11_HOME_BLOCK, hb) == 0) {
            fmt = rt11_datefmt_from_sysver(rd_u16(hb, HB_SYSVER_OFF));
        }
        date_w = rt11_encode_date(y, m, dy, fmt);
    }

    prev_size = found_size;
    if (prev_size == nblocks) {
        /* exact fit: just overwrite the UNUSED entry */
        wr_u16(dir, found_off,     RT11_E_PERMANENT);
        wr_u16(dir, found_off + 2, fn[0]);
        wr_u16(dir, found_off + 4, fn[1]);
        wr_u16(dir, found_off + 6, fn[2]);
        wr_u16(dir, found_off + 8, (uint16_t)nblocks);
        wr_u16(dir, found_off + 10, 0);
        wr_u16(dir, found_off + 12, date_w);
        new_dir_len = (uint32_t)dir_len;
        new_dir = dir;
        dir = NULL;
    } else {
        /* split: new PERMANENT entry + shrunken UNUSED after it */
        uint32_t remaining = prev_size - nblocks;
        new_dir_len = (uint32_t)dir_len + entry_bytes;
        new_dir = (uint8_t *)malloc(new_dir_len);
        if (!new_dir) { fprintf(stderr, "?Out of memory\n"); goto fail; }

        /* copy everything up to (but not including) found_off */
        memcpy(new_dir, dir, found_off);
        /* write the new PERMANENT entry */
        wr_u16(new_dir, found_off, RT11_E_PERMANENT);
        wr_u16(new_dir, found_off + 2, fn[0]);
        wr_u16(new_dir, found_off + 4, fn[1]);
        wr_u16(new_dir, found_off + 6, fn[2]);
        wr_u16(new_dir, found_off + 8, (uint16_t)nblocks);
        wr_u16(new_dir, found_off + 10, 0);
        wr_u16(new_dir, found_off + 12, date_w);
        /* zero extra bytes */
        if (entry_bytes > RT11_ENTRY_BASE_BYTES) {
            memset(new_dir + found_off + RT11_ENTRY_BASE_BYTES, 0,
                   entry_bytes - RT11_ENTRY_BASE_BYTES);
        }
        /* emit the shrunken UNUSED entry */
        {
            uint32_t nu = found_off + entry_bytes;
            wr_u16(new_dir, nu, RT11_E_UNUSED);
            wr_u16(new_dir, nu + 2, 0);
            wr_u16(new_dir, nu + 4, 0);
            wr_u16(new_dir, nu + 6, 0);
            wr_u16(new_dir, nu + 8, (uint16_t)remaining);
            wr_u16(new_dir, nu + 10, 0);
            wr_u16(new_dir, nu + 12, 0);
            if (entry_bytes > RT11_ENTRY_BASE_BYTES) {
                memset(new_dir + nu + RT11_ENTRY_BASE_BYTES, 0,
                       entry_bytes - RT11_ENTRY_BASE_BYTES);
            }
        }
        /* copy the rest of the directory, shifted one entry down */
        memcpy(new_dir + found_off + entry_bytes * 2,
               dir + found_off + entry_bytes,
               dir_len - (found_off + entry_bytes));
        free(dir);
        dir = NULL;
    }

    /* Write the updated directory. */
    if (store_dir_linear(d, new_dir, new_dir_len, entry_bytes,
                         total_segs, first_file_blk) != 0) {
        free(new_dir);
        return -1;
    }
    free(new_dir);

    printf("Copied '%s' -> %s (%u blocks)\n",
           host_path, dv_name, nblocks);
    return 0;

ioerr:
    fprintf(stderr, "?I/O error on '%s'\n", host_path);
fail:
    if (fp)  fclose(fp);
    if (buf) free(buf);
    if (dir) free(dir);
    return -1;
}

/* ======================================================================
 *   COPY   -   DV -> host file
 * ====================================================================== */

/* Forward declaration, implemented further down. */
static int find_file_ex(Rt11Dev *d, const char *name,
                        uint32_t *out_start, uint32_t *out_len);

int rt11_copy_out(Rt11Dev *d, const char *dv_name, const char *host_path) {
    uint32_t start_blk = 0;
    uint32_t nblocks   = 0;
    int r;
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    uint32_t i;

    if (!dv_name || !*dv_name) {
        fprintf(stderr, "?Missing RT-11 source filename\n");
        return -1;
    }
    if (!host_path || !*host_path) {
        fprintf(stderr, "?Missing host destination path\n");
        return -1;
    }
    if (!rt11_is_formatted(d)) {
        fprintf(stderr, "?Volume is not formatted as RT-11 "
                        "(use FORMAT first)\n");
        return -1;
    }

    r = find_file_ex(d, dv_name, &start_blk, &nblocks);
    if (r < 0) return -1;
    if (r == 0) {
        fprintf(stderr, "?File '%s' not found on DV\n", dv_name);
        return -1;
    }

    fp = fopen(host_path, "wb");
    if (!fp) {
        fprintf(stderr, "?Cannot create '%s': %s\n",
                host_path, strerror(errno));
        return -1;
    }

    buf = (uint8_t *)malloc(RT11_BLOCK_SIZE);
    if (!buf) {
        fprintf(stderr, "?Out of memory\n");
        fclose(fp);
        return -1;
    }

    for (i = 0; i < nblocks; i++) {
        if (rt11_read_block(d, start_blk + i, buf) != 0) goto fail;
        if (fwrite(buf, 1, RT11_BLOCK_SIZE, fp) != RT11_BLOCK_SIZE) {
            fprintf(stderr, "?Write error on '%s'\n", host_path);
            goto fail;
        }
    }

    free(buf);
    fclose(fp);
    printf("Copied %s -> '%s' (%u blocks)\n", dv_name, host_path, nblocks);
    return 0;

fail:
    free(buf);
    fclose(fp);
    remove(host_path);
    return -1;
}

/* ======================================================================
 *   BOOT
 * ====================================================================== */

/* Find a PERMANENT file by RAD50 name. Fills start_block and length.
 * Returns 1 on success, 0 on not-found, <0 on error. */
static int find_file_ex(Rt11Dev *d, const char *name,
                        uint32_t *out_start, uint32_t *out_len) {
    Rt11DirIter it;
    Rt11DirEntry e;
    uint16_t fn[3];
    int r;

    if (rad50_encode_filename(name, fn) != 0) return -1;

    if (rt11_dir_begin(d, &it) != 0) return -1;
    for (;;) {
        r = rt11_dir_next(&it, &e);
        if (r < 0) return -1;
        if (r == 0) return 0;
        if ((e.status & RT11_E_PERMANENT) &&
            e.fn_hi == fn[0] && e.fn_lo == fn[1] && e.ext == fn[2]) {
            *out_start = e.start_block;
            *out_len   = e.length;
            return 1;
        }
    }
}

/* Offsets in the bootstrap buffer (from the reference assembler). */
#define BOOT_SEC_BASE      01000    /* buffer offset for secondary boot */
#define BOOT_PATCH_DEVNAME 04716    /* B$DEVN (in ES buffer)           */
#define BOOT_PATCH_MONNAME 04724    /* monitor filename                */
#define BOOT_PATCH_BREAD   04730    /* B$READ offset                   */

int rt11_boot(Rt11Dev *d, const char *monitor_name, const char *handler_name) {
    uint32_t mon_start, mon_len;
    uint32_t hnd_start, hnd_len;
    uint16_t mfn[3];
    uint16_t hfn[3];
    uint8_t  *big = NULL;     /* full bootstrap buffer: blks 0..5 (=6 blks) */
    size_t    big_sz = 6 * RT11_BLOCK_SIZE;
    uint8_t   hblk0[RT11_BLOCK_SIZE];
    uint16_t  drv_addr, drv_len, bread_off;
    uint32_t  first_blk, last_blk, nblks;
    uint8_t   drvbuf[2 * RT11_BLOCK_SIZE];
    int       r;

    if (rad50_encode_filename(monitor_name, mfn) != 0) {
        fprintf(stderr, "?Bad monitor name '%s'\n", monitor_name);
        return -1;
    }
    if (rad50_encode_filename(handler_name, hfn) != 0) {
        fprintf(stderr, "?Bad handler name '%s'\n", handler_name);
        return -1;
    }
    if (!rt11_is_formatted(d)) {
        fprintf(stderr, "?Volume is not formatted as RT-11 "
                        "(use FORMAT first)\n");
        return -1;
    }

    r = find_file_ex(d, monitor_name, &mon_start, &mon_len);
    if (r <= 0) {
        fprintf(stderr, "?Monitor file '%s' not found on DV\n", monitor_name);
        return -1;
    }
    r = find_file_ex(d, handler_name, &hnd_start, &hnd_len);
    if (r <= 0) {
        fprintf(stderr, "?Handler file '%s' not found on DV\n", handler_name);
        return -1;
    }

    big = (uint8_t *)calloc(1, big_sz);
    if (!big) { fprintf(stderr, "?Out of memory\n"); return -1; }

    /* Load blocks 1..4 of the monitor into buffer starting at offset 01000
     * (i.e., write them into local blocks 1..4 of the bootstrap image). */
    if (mon_len < 5) {
        fprintf(stderr, "?Monitor too small (%u blks)\n", mon_len);
        goto fail;
    }
    if (rt11_read_blocks(d, mon_start + 1, 4,
                         big + BOOT_SEC_BASE) != 0) goto fail;

    /* Patch monitor filename at offset 04724 in the big buffer. */
    wr_u16(big, BOOT_PATCH_MONNAME,     mfn[0]);
    wr_u16(big, BOOT_PATCH_MONNAME + 2, mfn[1]);

    /* Read block 0 of the handler. */
    if (rt11_read_block(d, hnd_start, hblk0) != 0) goto fail;

    /* Driver metadata is at offsets 062, 064, 066 (octal) in that block. */
    drv_addr  = rd_u16(hblk0, 062);
    drv_len   = rd_u16(hblk0, 064);
    bread_off = rd_u16(hblk0, 066);
    if (drv_len == 0 || drv_len > RT11_BLOCK_SIZE) {
        fprintf(stderr, "?Corrupt device handler (len=%u)\n", drv_len);
        goto fail;
    }

    /* Compute which blocks of the handler file contain the primary driver. */
    first_blk = (uint32_t)drv_addr / RT11_BLOCK_SIZE;
    last_blk  = (uint32_t)(drv_addr + drv_len - 1) / RT11_BLOCK_SIZE;
    nblks     = last_blk - first_blk + 1;
    if (first_blk + nblks > hnd_len) {
        fprintf(stderr, "?Corrupt device handler (overruns file)\n");
        goto fail;
    }
    if (nblks > 2) {
        fprintf(stderr, "?Primary driver spans %u blocks (expected <=2)\n",
                nblks);
        goto fail;
    }

    if (rt11_read_blocks(d, hnd_start + first_blk, nblks, drvbuf) != 0)
        goto fail;

    /* Patch B$READ offset and device name in the big buffer. */
    wr_u16(big, BOOT_PATCH_BREAD,       bread_off);
    wr_u16(big, BOOT_PATCH_DEVNAME,     hfn[0]);
    /* Name is 3 words in rad50 but only the first word is copied here to
     * match the reference code (B$DEVN is a single word-sized device). */

    /* Copy the primary driver bytes into the start of the big buffer
     * (offset is drv_addr modulo block size, relative within drvbuf). */
    {
        uint32_t src = drv_addr - (first_blk * RT11_BLOCK_SIZE);
        if (src + drv_len > nblks * RT11_BLOCK_SIZE) {
            fprintf(stderr, "?Corrupt device handler (bad addr)\n");
            goto fail;
        }
        memcpy(big, drvbuf + src, drv_len);
        /* remainder of block 0 stays zero - it was already calloc'd */
    }

    /* Write block 0 (primary boot) and blocks 2..5 (secondary boot). */
    if (rt11_write_block (d, 0, big + 0) != 0) goto fail;
    if (rt11_write_blocks(d, 2, 4, big + BOOT_SEC_BASE) != 0) goto fail;

    free(big);
    printf("Bootstrap written using monitor '%s' and handler '%s'.\n",
           monitor_name, handler_name);
    return 0;

fail:
    free(big);
    return -1;
}
