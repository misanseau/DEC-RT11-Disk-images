/*
 * rsts.h - DEC RSTS/E disk pack support (read-only).
 *
 * RSTS/E packs (RDS 0.0 .. 1.X) consist of:
 *   - Boot block at LBN 0
 *   - Pack label / MFD label at DCN 1 (= LBN 1 * DCS).  In RDS 0.0
 *     this block is the start of the MFD itself; in RDS 1.X it is a
 *     dedicated pack label block and the MFD lives elsewhere.
 *   - Storage allocation table SATT.SYS as file [0,1].
 *   - MFD = Master File Directory; one entry per file account [P,Q].
 *     Each entry points (via DCN retrieval pointers) to a UFD which
 *     in turn lists the actual files.
 *
 * This first cut of the decoder implements just enough to:
 *   1. Sniff a disk image and decide "yes, this is RSTS/E".
 *   2. Decode the pack label / MFD label header (pack ID, PCS, RDS).
 *   3. List the PPNs of accounts visible in the MFD's first cluster.
 *
 * Full GFD / UFD walking, file-by-file listing and write paths are
 * left as future work -- the on-disk format is documented in
 *   - PUTR.ASM (J. Bohme) - the practical reference.
 *   - "RSTS/E Internals Manual" (Mayfield).
 *   - "RSTS/E System Manager's Guide" - DSKINT chapter.
 *
 * All multi-byte values are little-endian; offsets/fields below are
 * given in *decimal* unless noted otherwise.
 */
#ifndef RT11DV_RSTS_H
#define RT11DV_RSTS_H

#include <stdint.h>
#include <stdio.h>

#include "mount.h"

/* --- Geometry ------------------------------------------------------- */

#define RSTS_BLOCK_SIZE    512u
#define RSTS_BLOCKETTE     16u    /* every directory block holds 32 of these */
#define RSTS_BLKTS_PER_BLK 32u
#define RSTS_MFD_DCN       1u     /* DCN of pack label / MFD start          */

/* --- Pack-label / MFD-label byte offsets (decimal!) ----------------- */

#define RSTS_PL_LINK_OFF      0   /* link word                             */
#define RSTS_PL_USED_OFF      2   /* must be 0xFFFF                        */
#define RSTS_PL_RSVD_OFF      4   /* reserved (RDS 1.X may put data here)  */
#define RSTS_PL_RDS_OFF       6   /* 0=RDS 0.0, 0x01XX=RDS 1.X             */
#define RSTS_PL_PCS_OFF       8   /* pack cluster size (power of 2)        */
#define RSTS_PL_PSTAT_OFF    10   /* pack status bits                      */
#define RSTS_PL_ID1_OFF      12   /* pack ID, RAD-50 word 1 (3 chars)      */
#define RSTS_PL_ID2_OFF      14   /* pack ID, RAD-50 word 2 (3 chars)      */

/* --- Decoded pack label view --------------------------------------- */

typedef struct {
    uint16_t link;
    uint16_t used_flag;       /* should be 0xFFFF for a valid pack         */
    uint16_t reserved;
    uint16_t rds_level;       /* 0=0.0, 0x01nn=1.nn                        */
    uint16_t pcs;             /* pack cluster size (1, 2, 4, 8, 16, ...)   */
    uint16_t pack_status;
    char     pack_id[7];      /* "SYSGNG" + NUL                            */

    /* Computed by the decoder */
    uint32_t total_blocks;
    uint32_t dcs;             /* device cluster size                       */
    uint32_t mfd_lbn;         /* LBN of the first MFD block                */

    /* Validation flags */
    int      used_ok;         /* used_flag == 0xFFFF                       */
    int      pcs_ok;          /* PCS power of two and 1..64                */
    int      rds_ok;          /* RDS level recognized                      */
    int      id_ok;           /* pack_id[] all printable ASCII             */
} rsts_pack_t;

/* --- Public API ----------------------------------------------------- */

/* Compute the device cluster size (DCS) for a pack of `blocks` blocks.
 * DCS is the smallest power of two that brings cluster count below
 * 65536. Always returns at least 1. */
uint32_t rsts_dcs_for_size(uint32_t blocks);

/* Parse the pack label / MFD-label block at buf[0..511] into *out.
 * Always populates *out; returns 1 if the block looks like a valid
 * RSTS/E pack label (USED flag, recognised RDS, sane PCS, printable
 * ID), else 0. */
int rsts_parse_pack_label(const uint8_t *buf, uint32_t total_blocks,
                          rsts_pack_t *out);

/* Quick yes/no test over a mounted DV image: read DCN 1 (= LBN 1*DCS)
 * and check whether it parses as a RSTS pack label.  Returns 1 if yes,
 * 0 if no. */
int rsts_test(FILE *fp, uint32_t total_blocks);

/* Print a DIR-style report of an RSTS/E pack to stdout.
 *
 * uic_arg == NULL or empty: full walk -- pack header, MFD account list,
 *                           and a recursive descent into every account's
 *                           UFD listing files.
 * uic_arg == "[g,p]"      : list only that one UFD (no MFD/header).
 */
int rsts_cmd_dir(Mount *m, const char *uic_arg);

/* Extract one file from a mounted RSTS/E pack to a host path.
 *
 * Locates the named file under PPN [g,p] by walking the MFD to find
 * the account's UFD, then walking the UFD to find the Name Entry
 * matching name.ext (case-insensitive 6.3, RAD-50 encoded internally).
 * Follows the Retrieval Entry chain to read every file cluster from
 * the disk (DCN * DCS LBN, FCS contiguous blocks) and writes the
 * contents to host_path.
 *
 * Returns 0 on success, -1 on any error (with a stderr message).
 *
 * Caveats: read-only; URTS=0 large-file convention is honoured for
 * the byte-count cap; sparse / hole-filled files are not detected. */
int rsts_copy_out(Mount *m, int g, int p,
                  const char *name, const char *type,
                  const char *host_path);

/* Wildcard variant of rsts_copy_out.  Walks the UFD for [g,p], glob-
 * matches each file's name + type against name_pat / type_pat (where
 * '*' = zero+ chars and '?' = one char, case-insensitive), and writes
 * each match into host_dir.  host_dir should end in '/' or '\\' for a
 * directory; otherwise it is used as a literal prefix concatenated to
 * the RT-11-style "NAME.EXT" filename.
 *
 * case_mode: 0 = uppercase host names (default), 1 = lowercase,
 *            2 = no case conversion.
 *
 * Returns 0 on success.  *out_copied / *out_skipped / *out_errors get
 * the per-file totals when non-NULL. */
int rsts_copy_wild(Mount *m, int g, int p,
                   const char *name_pat, const char *type_pat,
                   const char *host_dir, int case_mode,
                   int *out_copied, int *out_skipped, int *out_errors);

/* Append a host file as a new entry in UFD [g,p].  Allocates storage
 * clusters via SATT.SYS, builds a Name Entry + Accounting Entry +
 * Retrieval Entry trio in the UFD, links the new Name Entry into the
 * UFD chain, and writes the host data.  If `dry_run` is non-zero the
 * function only prints the plan and does NOT modify the disk image.
 *
 * Limitations: file fits in <=7 file clusters (single Retrieval
 * Entry); UFD must already have 3 free blockettes; FCS = PCS; single
 * contiguous PCN run; no version handling; no name-collision check. */
int rsts_copy_in(Mount *m, int g, int p,
                 const char *name, const char *type,
                 const char *host_path, int dry_run);

#endif /* RT11DV_RSTS_H */
