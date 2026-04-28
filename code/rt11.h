/*
 * rt11.h - RT-11 / TSX+ on-disk structures and high-level FS operations.
 *
 * Summary of the layout produced/understood by this module:
 *
 *   block  0        : boot block (primary bootstrap, 1 block, all zero
 *                     right after CREATE/FORMAT).
 *   block  1        : home block.
 *   block  2..5     : secondary bootstrap area (zero after FORMAT).
 *   block  6..67    : 31 directory segments of 2 blocks each.
 *                     Only segment 1 is in use right after FORMAT.
 *   block 68..20479 : data area (20412 blocks = ~10 MB useful space).
 *
 * All numeric fields on disk are little-endian 16-bit words (same byte
 * order as the PDP-11 and x86).
 */
#ifndef RT11DV_RT11_H
#define RT11DV_RT11_H

#include <stdint.h>
#include <stdio.h>

/* --- On-disk geometry --------------------------------------------------- */

#define RT11_BLOCK_SIZE       512u                 /* bytes per block    */
#define RT11_WORDS_PER_BLOCK  256u                 /* 16-bit words/block */
#define RT11_DV_BYTES         (10u * 1024u * 1024u) /* 10 MB exact       */
#define RT11_DV_BLOCKS        (RT11_DV_BYTES / RT11_BLOCK_SIZE) /* 20480 */

#define RT11_HOME_BLOCK       1u
#define RT11_DIR_START_BLOCK  6u
#define RT11_MAX_SEGMENTS     31u
#define RT11_SEG_BLOCKS       2u
#define RT11_SEG_BYTES        (RT11_SEG_BLOCKS * RT11_BLOCK_SIZE)
#define RT11_SEG_HEADER_WORDS 5u
#define RT11_ENTRY_BASE_BYTES 14u                  /* before extra bytes */

/* --- Directory entry status bits --------------------------------------- */

#define RT11_E_TENTATIVE  0x0100u   /* octal   400 */
#define RT11_E_UNUSED     0x0200u   /* octal  1000 */
#define RT11_E_PERMANENT  0x0400u   /* octal  2000 */
#define RT11_E_ENDSEG     0x0800u   /* octal  4000 */
#define RT11_E_PROTECTED  0x8000u   /* octal 100000 */

/* --- Home-block byte offsets (from DEC V&FF manual) -------------------- */

#define HB_CLUSTER_OFF     0722  /* pack cluster size word                */
#define HB_DIR_BLK_OFF     0724  /* blk # of 1st dir seg (=6)            */
#define HB_SYSVER_OFF      0726  /* RAD50 'V05'                           */
#define HB_VOLID_OFF       0730  /* 12 ASCII bytes                        */
#define HB_OWNER_OFF       0744  /* 12 ASCII bytes                        */
#define HB_SYSID_OFF       0760  /* 12 ASCII bytes "DECRT11A "            */
#define HB_CHECKSUM_OFF    0776  /* final word - sum of prior 255 words  */

/* --- High-level types -------------------------------------------------- */

typedef struct {
    FILE       *fp;           /* open FILE* for the underlying image file */
    const char *path;         /* for diagnostics */
    uint32_t    total_blocks; /* size of the image, in 512-byte blocks */
} Rt11Dev;

/* A single directory entry as we expose it to the caller. */
typedef struct {
    uint16_t status;      /* STATUS bits */
    uint16_t fn_hi;       /* RAD50 filename word 1 */
    uint16_t fn_lo;       /* RAD50 filename word 2 */
    uint16_t ext;         /* RAD50 extension word  */
    uint16_t length;      /* size of entry in blocks */
    uint8_t  channel;
    uint8_t  job;
    uint16_t date;        /* raw RT-11 date word */
    /* Metadata (not on disk, computed on the fly): */
    uint32_t start_block; /* abs block # where this entry's data starts */
    uint32_t seg_no;      /* 1..31, segment that contains this entry */
    uint32_t seg_offset;  /* byte offset inside the segment buffer */
} Rt11DirEntry;

/* --- Block-level I/O (absolute block numbers) -------------------------- */

int rt11_read_block (Rt11Dev *d, uint32_t blk, void *buf);
int rt11_write_block(Rt11Dev *d, uint32_t blk, const void *buf);

int rt11_read_blocks (Rt11Dev *d, uint32_t blk, uint32_t n, void *buf);
int rt11_write_blocks(Rt11Dev *d, uint32_t blk, uint32_t n, const void *buf);

/* --- Volume operations ------------------------------------------------- */

/* Returns 1 iff the DV looks like it has been FORMATted (valid home-block
 * SYSID and a sane directory segment 1 header).  Returns 0 otherwise. */
int rt11_is_formatted(Rt11Dev *d);

/* CREATE: produce a brand-new image file of 10 MB, all zero bytes. */
int rt11_create_image(const char *path);

/* FORMAT: initialise an already-opened DV with an empty RT-11 directory.
 *    - writes home block
 *    - writes 31 empty directory segments (only seg 1 in use)
 *    - leaves data area untouched. */
int rt11_format(Rt11Dev *d, const char *vol_id, const char *owner_name);

/* --- Directory enumeration --------------------------------------------- */

typedef struct {
    Rt11Dev *dev;
    uint8_t  segbuf[RT11_SEG_BYTES];
    uint16_t extra_bytes;  /* from first segment header */
    uint32_t seg_no;       /* current segment # (1-based) */
    uint32_t cur_offset;   /* offset into segbuf for next entry */
    uint32_t cur_block;    /* running start-block of next entry's data */
    uint16_t total_segs;
} Rt11DirIter;

int rt11_dir_begin(Rt11Dev *d, Rt11DirIter *it);
/* Returns 1 if an entry was returned in *e, 0 at end of dir, <0 on error. */
int rt11_dir_next (Rt11DirIter *it, Rt11DirEntry *e);

/* --- Higher-level operations ------------------------------------------- */

/* DIR listing on stdout (also used to confirm the volume is formatted). */
int rt11_cmd_dir(Rt11Dev *d);

/* Copy a host file into the DV. Returns 0 on success.
 * If dv_name is NULL or empty, the RT-11 filename is derived from the
 * basename of host_path. */
int rt11_copy_in (Rt11Dev *d, const char *host_path, const char *dv_name);

/* Copy a file out of the DV onto the host filesystem.  The file is
 * extracted as whole blocks (RT-11 tracks files in 512-byte blocks; no
 * byte-length metadata is stored on disk).  Returns 0 on success. */
int rt11_copy_out(Rt11Dev *d, const char *dv_name, const char *host_path);

/* Write a fully-patched RT-11 bootstrap to blocks 0 and 2-5 using the
 * named monitor and handler files already stored on the DV.  Both files
 * must exist in the directory before calling this function. */
int rt11_boot(Rt11Dev *d, const char *monitor_name, const char *handler_name);

#endif /* RT11DV_RT11_H */
