/*
 * ods1.h - Files-11 ODS-1 (RSX-11D / RSX-11M / IAS / VAX initialised
 *          for compatibility) on-disk structure decoders.
 *
 * Pure read-side, decoder-only API.  No I/O is done here -- callers
 * pass in 512-byte block buffers and we report what's in them.
 *
 * References:
 *   Files-11 ODS-1 Specification, June 1975 (DEC, RSX-11D)
 *   Files-11 ODS-1 Specification, Sep 1986 reprint
 *   Files11.cs by Kenneth Gober (MIT-licensed FSX implementation)
 *   files11.txt - FSX extract previously imported into the project
 *
 * On-disk layout overview (per spec):
 *
 *   block 0           : primary boot
 *   block 1           : home block (super block)
 *   block 2           : reserved
 *   ...               : remaining boot loader area
 *   IBLB              : start of index file bitmap (size IBSZ blocks)
 *   IBLB+IBSZ         : 16 reserved file headers (numbers 1..16)
 *                       1=INDEXF.SYS, 2=BITMAP.SYS, 3=BADBLK.SYS,
 *                       4=000000.DIR, 5=CORIMG.SYS
 *   IBLB+IBSZ+16+...  : remaining index file blocks (file headers 17+)
 *
 * Multi-byte fields on disk are little-endian, except for the 32-bit
 * IBLB pointer in the home block, which DEC stores as <hi-word><lo-word>
 * (a quirk of the original RSX-11D code).
 */
#ifndef RT11DV_ODS1_H
#define RT11DV_ODS1_H

#include <stdint.h>
#include <stddef.h>

/* --- Geometry --------------------------------------------------------- */

#define ODS1_BLOCK_SIZE        512u
#define ODS1_HOME_BLOCK        1u

/* --- Home block byte offsets (per spec / files11.txt lines 47-71) ----- */

#define ODS1_HB_IBSZ_OFF       0    /* H.IBSZ  index file bitmap size       */
#define ODS1_HB_IBLB_HI_OFF    2    /* H.IBLB  hi word of bitmap LBN        */
#define ODS1_HB_IBLB_LO_OFF    4    /*         lo word of bitmap LBN        */
#define ODS1_HB_FMAX_OFF       6    /* H.FMAX  max number of files          */
#define ODS1_HB_SBCL_OFF       8    /* H.SBCL  storage bitmap cluster (==1) */
#define ODS1_HB_DVTY_OFF       10   /* H.DVTY  device type     (==0)        */
#define ODS1_HB_VLEV_OFF       12   /* H.VLEV  volume structure level       */
#define ODS1_HB_VNAM_OFF       14   /* H.VNAM  12-byte name (null-padded)   */
#define ODS1_HB_VOWN_OFF       30   /* H.VOWN  owner UIC (programmer/group) */
#define ODS1_HB_VPRO_OFF       32   /* H.VPRO  volume protection            */
#define ODS1_HB_VCHA_OFF       34   /* H.VCHA  characteristics              */
#define ODS1_HB_DFPR_OFF       36   /* H.DFPR  default file protection      */
#define ODS1_HB_WISZ_OFF       44   /* H.WISZ  default window size (byte)   */
#define ODS1_HB_FIEX_OFF       45   /* H.FIEX  default file extend (byte)   */
#define ODS1_HB_LRUC_OFF       46   /* H.LRUC  directory pre-access limit   */
#define ODS1_HB_CHK1_OFF       58   /* H.CHK1  first checksum (words 0..28) */
#define ODS1_HB_VDAT_OFF       60   /* H.VDAT  "DDMMMYYHHMMSS" (13 bytes)   */
#define ODS1_HB_INDN_OFF       472  /* H.INDN  vol name, space-padded (12)  */
#define ODS1_HB_INDO_OFF       484  /* H.INDO  owner,    space-padded (12)  */
#define ODS1_HB_INDF_OFF       496  /* H.INDF  "DECFILE11A" + 2 spaces (12) */
#define ODS1_HB_CHK2_OFF       510  /* H.CHK2  second checksum (0..254)     */

#define ODS1_HB_VLEV_VALUE_V1  0x0101u  /* ODS-1 structure level            */
#define ODS1_HB_VLEV_VALUE_V12 0x0102u  /* IAS extension                    */
#define ODS1_HB_FORMAT_SIG     "DECFILE11A  "  /* exactly 12 bytes in INDF  */

/* --- File header byte offsets (block sized 512) ----------------------- */

#define ODS1_FH_IDOF_OFF       0    /* H.IDOF  ident area offset (words)   */
#define ODS1_FH_MPOF_OFF       1    /* H.MPOF  map   area offset (words)   */
#define ODS1_FH_FNUM_OFF       2    /* H.FNUM  file number                 */
#define ODS1_FH_FSEQ_OFF       4    /* H.FSEQ  file sequence number        */
#define ODS1_FH_FLEV_OFF       6    /* H.FLEV  file structure level        */
#define ODS1_FH_FOWN_OFF       8    /* H.FOWN  file owner UIC              */
#define ODS1_FH_FPRO_OFF       10   /* H.FPRO  file protection             */
#define ODS1_FH_FCHA_OFF       12   /* H.FCHA  characteristics             */
#define ODS1_FH_UFAT_OFF       14   /* H.UFAT  user attribute area (32 B)  */
#define ODS1_FH_CKSM_OFF       510  /* checksum over words 0..254          */

#define ODS1_FH_FLEV_VALUE     0x0101u

/* User attribute area sub-fields (FCS-11) */
#define ODS1_UFAT_RTYP_OFF     0   /* F.RTYP  record type                  */
#define ODS1_UFAT_RATT_OFF     1   /* F.RATT  record attributes (FD.CR=2)  */
#define ODS1_UFAT_RSIZ_OFF     2   /* F.RSIZ  record size                  */
#define ODS1_UFAT_HIBK_OFF     4   /* F.HIBK  highest VBN allocated        */
#define ODS1_UFAT_EFBK_OFF     8   /* F.EFBK  end-of-file block            */
#define ODS1_UFAT_FFBY_OFF     12  /* F.FFBY  first free byte (word)       */

#define ODS1_RTYP_FIX          1u  /* R.FIX  fixed-length records          */
#define ODS1_RTYP_VAR          2u  /* R.VAR  variable-length               */
#define ODS1_RATT_FD_CR        2u  /* implied carriage return / line feed  */

/* --- Ident area sub-field offsets (relative to ident area start) ------ */

#define ODS1_ID_FNAM_OFF       0   /* I.FNAM  9 chars (3 RAD50 words)      */
#define ODS1_ID_FTYP_OFF       6   /* I.FTYP  3 chars (1 RAD50 word)       */
#define ODS1_ID_FVER_OFF       8   /* I.FVER  signed version number        */
#define ODS1_ID_RVNO_OFF       10  /* I.RVNO  revision number              */
#define ODS1_ID_RVDT_OFF       12  /* I.RVDT  'ddMMMyy' revision date      */
#define ODS1_ID_RVTI_OFF       19  /* I.RVTI  'HHmmss'  revision time      */
#define ODS1_ID_CRDT_OFF       25  /* I.CRDT  'ddMMMyy' creation date      */
#define ODS1_ID_CRTI_OFF       32  /* I.CRTI  'HHmmss'  creation time      */
#define ODS1_ID_EXDT_OFF       38  /* I.EXDT  'ddMMMyy' expiration date    */

/* --- Map area sub-field offsets (relative to map area start) ---------- */

#define ODS1_MAP_ESQN_OFF      0   /* M.ESQN  extension segment number     */
#define ODS1_MAP_ERVN_OFF      1   /* M.ERVN  extension RVN                */
#define ODS1_MAP_EFNU_OFF      2   /* M.EFNU  next extension file number   */
#define ODS1_MAP_EFSQ_OFF      4   /* M.EFSQ  next extension seq number    */
#define ODS1_MAP_CTSZ_OFF      6   /* M.CTSZ  count   field size (bytes)   */
#define ODS1_MAP_LBSZ_OFF      7   /* M.LBSZ  LBN     field size (bytes)   */
#define ODS1_MAP_USE_OFF       8   /* M.USE   map words in use             */
#define ODS1_MAP_MAX_OFF       9   /* M.MAX   map words available          */
#define ODS1_MAP_RTRV_OFF      10  /* start of retrieval pointers          */

/* --- Reserved file numbers (ODS-1 standard) --------------------------- */

#define ODS1_FNUM_INDEX        1u
#define ODS1_FNUM_BITMAP       2u
#define ODS1_FNUM_BADBLK       3u
#define ODS1_FNUM_MASTER_DIR   4u
#define ODS1_FNUM_CORIMG       5u

/* --- Decoded views (host endian, ASCII-clean) ------------------------- */

typedef struct {
    /* Raw header fields */
    uint16_t ibsz;
    uint32_t iblb;            /* combined LBN of index bitmap (DEC big-half) */
    uint16_t fmax;
    uint16_t sbcl;
    uint16_t dvty;
    uint16_t vlev;
    char     vnam[13];        /* 12 + NUL (null-padded on disk)             */
    uint16_t vown;
    uint16_t vpro;
    uint16_t vcha;
    uint16_t dfpr;
    uint8_t  wisz;
    uint8_t  fiex;
    uint8_t  lruc;

    /* Identification block */
    char     vdat[14];        /* "DDMMMYYHHMMSS" + NUL                      */
    char     indn[13];        /* 12 + NUL (space-padded on disk)            */
    char     indo[13];
    char     indf[13];        /* "DECFILE11A  " + NUL                       */

    /* Checksums */
    uint16_t chk1;            /* stored                                     */
    uint16_t chk1_computed;
    uint16_t chk2;            /* stored                                     */
    uint16_t chk2_computed;

    /* Boolean validation results -- non-zero == OK */
    int      chk1_ok;
    int      chk2_ok;
    int      sig_ok;          /* INDF == "DECFILE11A  "                     */
    int      vlev_ok;          /* VLEV in {0x0101, 0x0102}                   */
    int      sbcl_ok;          /* SBCL == 1                                  */
    int      dvty_ok;          /* DVTY == 0                                  */
} ods1_home_t;

typedef struct {
    /* Header area */
    uint8_t  idof;            /* in words                                   */
    uint8_t  mpof;            /* in words                                   */
    uint16_t fnum;
    uint16_t fseq;
    uint16_t flev;
    uint16_t fown;
    uint16_t fpro;
    uint16_t fcha;
    /* User attribute area (FCS-11) */
    uint8_t  rtyp;
    uint8_t  ratt;
    uint16_t rsiz;
    uint32_t hibk;            /* high VBN allocated (32-bit)                */
    uint32_t efbk;            /* end-of-file block                          */
    uint16_t ffby;            /* first free byte                            */
    /* Ident area */
    char     fname[10];       /* 9 chars + NUL                              */
    char     ftype[4];        /* 3 chars + NUL                              */
    int16_t  fver;            /* signed                                     */
    uint16_t rvno;
    char     rvdt[8];         /* 7 chars + NUL "ddMMMyy"                    */
    char     rvti[7];         /* 6 chars + NUL "HHmmss"                     */
    char     crdt[8];
    char     crti[7];
    char     exdt[8];
    /* Map area summary */
    uint8_t  m_esqn;
    uint8_t  m_ervn;
    uint16_t m_efnu;
    uint16_t m_efsq;
    uint8_t  m_ctsz;
    uint8_t  m_lbsz;
    uint8_t  m_use;
    uint8_t  m_max;
    /* Checksum */
    uint16_t cksm;
    uint16_t cksm_computed;
    /* Validation flags */
    int      cksm_ok;
    int      flev_ok;
    int      offsets_ok;       /* idof <= mpof <= 256 (words)               */
} ods1_fh_t;

/* A single retrieval pointer expanded to host form. */
typedef struct {
    uint32_t lbn;             /* starting LBN of this run                   */
    uint32_t count;           /* number of contiguous blocks (count_field+1)*/
} ods1_retr_t;

/* Result of a Test() call (FSX style). */
typedef enum {
    ODS1_TEST_PASS  = 0,
    ODS1_TEST_FAIL  = 1
} ods1_test_result_t;

typedef struct {
    int                level_reached;  /* highest level that passed         */
    ods1_test_result_t result;
    int32_t            volume_size_blocks; /* -1 if not determinable        */
    char               reason[160];    /* if failed, why                    */
} ods1_test_t;

/* --- Public API (parsers operate on raw 512-byte buffers) ------------- */

/* Parse the home block at buf[0..511] into *out.  Always succeeds in
 * the sense that all fields are filled and validation flags reflect
 * whether each individual check passed.  Returns 1 if the volume looks
 * unambiguously like ODS-1 (signature OK and chk2 OK), else 0. */
int ods1_parse_home(const uint8_t *buf, ods1_home_t *out);

/* Parse a file header block into *out.  Returns 1 if checksum + flev
 * + offsets validate, else 0 (but *out is still populated). */
int ods1_parse_fh(const uint8_t *buf, ods1_fh_t *out);

/* Walk the retrieval pointers in a file header.  For each run, calls
 * cb(arg, &run, run_index).  Returns the number of runs walked, or
 * -1 on a malformed map area.  ctsz/lbsz come from out->m_ctsz/m_lbsz. */
typedef void (*ods1_retr_cb)(void *arg, const ods1_retr_t *r, uint32_t idx);
int ods1_walk_map(const uint8_t *buf, const ods1_fh_t *fh,
                  ods1_retr_cb cb, void *arg);

/* Compute LBN of the file header for a given file number using only
 * home-block info (works for fnum 1..16; returns 0 if fnum > 16 -- in
 * that case the caller has to traverse the index file). */
uint32_t ods1_fh_lbn(const ods1_home_t *hb, uint16_t fnum);

/* --- Directory entry (16 bytes per entry in any .DIR file) ------------ */

#define ODS1_DIRENT_BYTES      16

#define ODS1_DE_FNUM_OFF       0   /* file number              (word, LE) */
#define ODS1_DE_FSEQ_OFF       2   /* file sequence number     (word, LE) */
#define ODS1_DE_FRVN_OFF       4   /* relative volume number   (word, LE) */
#define ODS1_DE_NAME_OFF       6   /* 9 chars, 3 RAD50 words              */
#define ODS1_DE_TYPE_OFF       12  /* 3 chars, 1 RAD50 word               */
#define ODS1_DE_VERS_OFF       14  /* signed version           (word, LE) */

typedef struct {
    uint16_t fnum;
    uint16_t fseq;
    uint16_t frvn;
    char     name[10];        /* 9 chars + NUL                            */
    char     type[4];         /* 3 chars + NUL                            */
    int16_t  version;
} ods1_dirent_t;

/* Parse one 16-byte directory entry from `buf` into *out.  Returns 1 if
 * the slot is a real entry (fnum != 0), 0 if it's an unused/empty slot.
 * Always populates *out -- empty slots get zeroed fields. */
int ods1_parse_dirent(const uint8_t *buf, ods1_dirent_t *out);

/* RAD50 helpers: ODS-1 uses standard DEC RAD50 (same alphabet as RT-11),
 * but file names are 9 chars (3 words) and types are 3 chars (1 word).
 * Output buffers are filled with chars and NUL-terminated. */
void ods1_rad50_decode_name(uint16_t w0, uint16_t w1, uint16_t w2,
                            char out[10]);
void ods1_rad50_decode_type(uint16_t w, char out[4]);

/* Compute the additive 16-bit checksum used by both home block (twice)
 * and file header.  Sums words [0..(checksum_off/2 - 1)] mod 65536. */
uint16_t ods1_checksum(const uint8_t *buf, size_t checksum_off);

/* Auto-detection a la FSX, levels 0..2 (boot/home).  volume_blocks
 * is the number of 512-byte blocks in the image (or 0 if unknown).
 * For level >= 1 the caller must also pass home_block (block 1 read
 * into a 512-byte buffer); for level 0 it can be NULL. */
ods1_test_t ods1_test(int level, uint32_t volume_blocks,
                      const uint8_t *home_block);

/* Convenience: print a human-readable home-block report to fp.  Mirrors
 * the look of the existing RT-11 EXAM 1 output so they sit nicely
 * side-by-side. */
struct FILE_;
void ods1_print_home(void *fp, const ods1_home_t *h);

#endif /* RT11DV_ODS1_H */
