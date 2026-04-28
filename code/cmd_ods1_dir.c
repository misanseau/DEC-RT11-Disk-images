/*
 * cmd_ods1_dir.c - DIR command for Files-11 ODS-1 (RSX-11M / IAS / VAX
 *                  compatibility) volumes.
 *
 * Two listing modes:
 *
 *   DIR A:               -> walks the MFD (file [4,4] = 000000.DIR) and
 *                           prints every UFD entry as "[g,m]".
 *   DIR A:[g,m]          -> opens the matching UFD entry from the MFD,
 *                           reads its file-header retrieval pointers and
 *                           lists every file inside.
 *
 * The volume is detected as ODS-1 in `cmd_dir.c` (sniff block 1 with
 * ods1_test).  We only deal with random-access reads here.
 *
 * Limitations of this first cut:
 *   - INDEXF.SYS walking is NOT implemented; UFDs whose file number is
 *     <= 16 work directly via ods1_fh_lbn(); larger UFDs require an
 *     INDEXF lookup which is left as future work (and reported with a
 *     clear message rather than a wrong answer).
 *   - Map-area extension headers are not chased: if a UFD spans more
 *     than the first map area, we list what's reachable from header 0
 *     and warn at the end.  Real RSX UFDs almost always fit in one map.
 */
#include "cmd_internal.h"
#include "commands.h"
#include "mount.h"
#include "ods1.h"
#include "rt11.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------------
 *  Block I/O wrappers (random access into the .dsk image)
 * ---------------------------------------------------------------------- */

static int read_block(FILE *fp, uint32_t lbn, uint8_t *buf) {
    if (RT_FSEEK64(fp, (long long)lbn * 512LL, SEEK_SET) != 0) return -1;
    if (fread(buf, 1, 512, fp) != 512) return -1;
    return 0;
}

/* Little-endian 16-bit fetch (mirror of the helpers in rsts.c / ods1.c). */
static uint16_t le16(const uint8_t *buf, size_t off) {
    return (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
}

/* Read the home block (block 1) and parse it.  Returns 1 on success
 * (signature + checksum 2 OK), 0 on failure. */
static int load_home(FILE *fp, ods1_home_t *home) {
    uint8_t buf[512];
    if (read_block(fp, ODS1_HOME_BLOCK, buf) != 0) return 0;
    return ods1_parse_home(buf, home);
}

/* ----------------------------------------------------------------------
 *  Map-area walker helpers (declared early -- used by load_indexf below)
 * ---------------------------------------------------------------------- */

typedef struct {
    /* Output: collected runs of contiguous LBNs.  Cap at 64 runs per
     * directory file -- in practice an RSX UFD never has more than a
     * couple of extents. */
    ods1_retr_t runs[64];
    int         n;
    int         truncated;
} run_collector_t;

static void collect_run(void *arg, const ods1_retr_t *r, uint32_t idx) {
    run_collector_t *c = (run_collector_t *)arg;
    (void)idx;
    if (c->n < (int)(sizeof(c->runs) / sizeof(c->runs[0]))) {
        c->runs[c->n++] = *r;
    } else {
        c->truncated = 1;
    }
}

/* ----------------------------------------------------------------------
 *  INDEXF.SYS walker: cache the file's retrieval extents so any FID's
 *  file-header LBN can be computed via VBN -> LBN lookup.
 *
 *  Mapping: INDEXF.SYS contains, in VBN order:
 *      VBN 1            : boot block
 *      VBN 2            : home block
 *      VBN 3..2+IBSZ    : index file bitmap (IBSZ blocks)
 *      VBN 3+IBSZ ...   : file headers, starting with FID 1 at
 *                         VBN = 3 + IBSZ (header for INDEXF.SYS itself).
 *
 *  So for any FID N (>= 1), the file header's VBN is
 *      vbn(N) = N + IBSZ + 2
 *
 *  We just walk the cached retrieval list summing counts until we
 *  cover that VBN.
 * ---------------------------------------------------------------------- */

typedef struct {
    ods1_retr_t runs[64];
    int         n;
    int         truncated;
    uint32_t    ibsz;     /* copied from home block; cached for VBN math */
    int         loaded;
} indexf_t;

/* Forward decl: load_indexf needs a private bootstrap read of FID 1
 * before any INDEXF cache exists, so we can't call read_fh() here. */
static int load_indexf(FILE *fp, const ods1_home_t *home, indexf_t *ix);

static uint32_t indexf_lookup_lbn(const indexf_t *ix, uint16_t fnum) {
    /* vbn = fnum + ibsz + 2 (1-based VBN of the header in INDEXF.SYS) */
    uint32_t vbn = (uint32_t)fnum + ix->ibsz + 2u;
    uint32_t acc = 0;
    int      i;
    if (fnum == 0) return 0;
    for (i = 0; i < ix->n; i++) {
        if (vbn <= acc + ix->runs[i].count) {
            return ix->runs[i].lbn + (vbn - acc - 1u);
        }
        acc += ix->runs[i].count;
    }
    return 0;   /* fnum out of range of allocated INDEXF blocks */
}

/* Read the file header for a given file number.
 *
 *   - For fnum 1..16, the LBN is computed directly from home-block info
 *     (ods1_fh_lbn).  This works without an INDEXF cache, which lets us
 *     bootstrap the cache itself.
 *   - For fnum > 16, we use the INDEXF.SYS retrieval cache when available;
 *     otherwise we fall back to the historical naive linear formula
 *     (correct only when INDEXF.SYS is contiguous after the bitmap).
 *
 * On success returns 0 and fills *fh + the raw 512-byte buffer (the
 * latter is required by ods1_walk_map). */
static int read_fh(FILE *fp, const ods1_home_t *home, const indexf_t *ix,
                   uint16_t fnum,
                   uint8_t out_buf[512], ods1_fh_t *out_fh) {
    uint32_t lbn;
    if (fnum == 0) return -1;
    if (fnum <= 16) {
        lbn = ods1_fh_lbn(home, fnum);
    } else if (ix && ix->loaded) {
        lbn = indexf_lookup_lbn(ix, fnum);
        if (lbn == 0) return -1;
    } else {
        lbn = home->iblb + home->ibsz + (uint32_t)(fnum - 1);
    }
    if (read_block(fp, lbn, out_buf) != 0) return -1;
    if (!ods1_parse_fh(out_buf, out_fh))    return -1;
    if (out_fh->fnum != fnum)               return -1;
    return 0;
}

static int load_indexf(FILE *fp, const ods1_home_t *home, indexf_t *ix) {
    uint8_t   buf[512];
    ods1_fh_t fh;
    memset(ix, 0, sizeof(*ix));
    ix->ibsz = home->ibsz;
    /* Bootstrap: FID 1 is INDEXF.SYS, and FIDs 1..16 don't need the
     * INDEXF cache to locate (ods1_fh_lbn is enough). */
    if (read_fh(fp, home, NULL, ODS1_FNUM_INDEX, buf, &fh) != 0) {
        return -1;
    }
    if (ods1_walk_map(buf, &fh, collect_run, ix) < 0) {
        return -1;
    }
    ix->loaded = 1;
    return 0;
}

/* ----------------------------------------------------------------------
 *  UFD-name decoder.  In an RSX MFD entry the "name" field holds a
 *  6-digit RAD-50 string of the form GGGMMM (e.g. "001001" for [1,1]).
 *  Returns 1 if the name decodes to a [g,m] pair, 0 otherwise.
 * ---------------------------------------------------------------------- */

static int ufd_name_to_uic(const char *name, int *g_out, int *m_out) {
    int  i, g, m;
    char buf[10];
    /* Trim trailing spaces. */
    for (i = 0; i < 9 && name[i]; i++) buf[i] = name[i];
    buf[i] = '\0';
    while (i > 0 && buf[i-1] == ' ') buf[--i] = '\0';
    if (i != 6) return 0;
    for (i = 0; i < 6; i++) {
        if (buf[i] < '0' || buf[i] > '9') return 0;
    }
    g = (buf[0] - '0') * 100 + (buf[1] - '0') * 10 + (buf[2] - '0');
    m = (buf[3] - '0') * 100 + (buf[4] - '0') * 10 + (buf[5] - '0');
    *g_out = g;
    *m_out = m;
    return 1;
}

/* Reverse of the above: format a UIC [g,m] back to "GGGMMM" string for
 * comparison with a UFD entry name.  Pads each side with zeros. */
static void uic_to_ufd_name(int g, int m, char out[7]) {
    snprintf(out, 7, "%03d%03d", g & 0xFF, m & 0xFF);
}

/* ----------------------------------------------------------------------
 *  Parse "[g,m]" or "[g,m]*.ext" out of a token.
 *  Returns 1 if it matched, 0 otherwise.
 * ---------------------------------------------------------------------- */

static int parse_uic_arg(const char *arg, int *g_out, int *m_out) {
    int  g = 0, m = 0;
    const char *p;
    if (!arg || arg[0] != '[') return 0;
    p = arg + 1;
    while (*p >= '0' && *p <= '9') { g = g * 10 + (*p - '0'); p++; }
    if (*p != ',') return 0;
    p++;
    while (*p >= '0' && *p <= '9') { m = m * 10 + (*p - '0'); p++; }
    if (*p != ']') return 0;
    *g_out = g;
    *m_out = m;
    return 1;
}

/* ----------------------------------------------------------------------
 *  Walk one directory file and call cb(arg, &dirent) for every used
 *  entry.  Returns 0 on success, -1 on error.
 * ---------------------------------------------------------------------- */

typedef int (*dirent_cb)(void *arg, const ods1_dirent_t *de);

static int walk_dir_file(FILE *fp, const ods1_home_t *home,
                         const indexf_t *ix, uint16_t dir_fid,
                         dirent_cb cb, void *arg) {
    uint8_t          fh_buf[512];
    ods1_fh_t        fh;
    run_collector_t  runs;
    uint32_t         total_blocks = 0;
    int              i, rc;

    if (read_fh(fp, home, ix, dir_fid, fh_buf, &fh) != 0) {
        fprintf(stderr, "?Cannot read file header for FID=%u\n",
                (unsigned)dir_fid);
        return -1;
    }
    runs.n = 0;
    runs.truncated = 0;
    if (ods1_walk_map(fh_buf, &fh, collect_run, &runs) < 0) {
        fprintf(stderr, "?Malformed map area in directory FID=%u\n",
                (unsigned)dir_fid);
        return -1;
    }
    if (runs.n == 0) {
        return 0;   /* empty directory */
    }
    for (i = 0; i < runs.n; i++) total_blocks += runs.runs[i].count;

    /* Walk every block of every run, parse 32 entries per block. */
    for (i = 0; i < runs.n; i++) {
        uint32_t lbn   = runs.runs[i].lbn;
        uint32_t count = runs.runs[i].count;
        uint32_t b;
        for (b = 0; b < count; b++) {
            uint8_t blk[512];
            int     k;
            if (read_block(fp, lbn + b, blk) != 0) {
                fprintf(stderr, "?Read error in directory at LBN %u\n",
                        (unsigned)(lbn + b));
                return -1;
            }
            for (k = 0; k < 512; k += ODS1_DIRENT_BYTES) {
                ods1_dirent_t de;
                if (ods1_parse_dirent(blk + k, &de) == 0) continue;
                rc = cb(arg, &de);
                if (rc != 0) return rc;
            }
        }
    }
    if (runs.truncated) {
        fprintf(stderr,
                "?Warning: directory has more extents than walker can hold "
                "(listing may be incomplete)\n");
    }
    return 0;
}

/* ----------------------------------------------------------------------
 *  MFD listing -- show every UFD as "[g,m]"
 * ---------------------------------------------------------------------- */

typedef struct {
    int  count;
    int  files;        /* non-UFD entries (the 5 reserved) */
} mfd_state_t;

static int mfd_cb(void *arg, const ods1_dirent_t *de) {
    mfd_state_t *s = (mfd_state_t *)arg;
    int g, m;

    /* Reserved files 1..5 sit alongside UFDs in the MFD; show them too
     * but mark them as "<reserved>" instead of [g,m]. */
    if (ufd_name_to_uic(de->name, &g, &m)) {
        printf("  [%3d,%3d]   %-9s.%-3s;%-3d   FID=[%u,%u]\n",
               g, m, de->name, de->type, (int)de->version,
               (unsigned)de->fnum, (unsigned)de->fseq);
        s->count++;
    } else {
        printf("  <reserved>  %-9s.%-3s;%-3d   FID=[%u,%u]\n",
               de->name, de->type, (int)de->version,
               (unsigned)de->fnum, (unsigned)de->fseq);
        s->files++;
    }
    return 0;
}

static int do_dir_mfd(FILE *fp, const ods1_home_t *home,
                      const indexf_t *ix, const Mount *m) {
    mfd_state_t s = {0, 0};
    printf(" Directory of %s  (Files-11 ODS-1, volume \"%s\")\n",
           m->path, home->vnam);
    printf("  Owner: %u,%u   Vol level: 0%o   Max files: %u\n\n",
           (unsigned)((home->vown >> 8) & 0xff),
           (unsigned)( home->vown       & 0xff),
           (unsigned)home->vlev,
           (unsigned)home->fmax);
    printf("  UIC         Filename       FID\n");
    printf("  ---------   -------------  -----------\n");
    if (walk_dir_file(fp, home, ix, ODS1_FNUM_MASTER_DIR, mfd_cb, &s) != 0) {
        return -1;
    }
    printf("\n  %d UFDs, %d reserved files\n", s.count, s.files);
    return 0;
}

/* ----------------------------------------------------------------------
 *  UFD listing -- "[g,m] -> *.SYS, *.MAC, ..."
 * ---------------------------------------------------------------------- */

typedef struct {
    int  count;
} ufd_state_t;

static int ufd_cb(void *arg, const ods1_dirent_t *de) {
    ufd_state_t *s = (ufd_state_t *)arg;
    printf("  %-9s.%-3s;%-3d    FID=[%u,%u]\n",
           de->name, de->type, (int)de->version,
           (unsigned)de->fnum, (unsigned)de->fseq);
    s->count++;
    return 0;
}

/* Find the FID of UFD [g,m] by walking the MFD.  Returns 0 on not-found. */
typedef struct {
    char     target[7];   /* "GGGMMM\0" */
    uint16_t found_fnum;
    uint16_t found_fseq;
} ufd_find_t;

static int find_ufd_cb(void *arg, const ods1_dirent_t *de) {
    ufd_find_t *f = (ufd_find_t *)arg;
    if (de->type[0] && strcmp(de->type, "DIR") == 0 &&
        strncmp(de->name, f->target, 6) == 0) {
        f->found_fnum = de->fnum;
        f->found_fseq = de->fseq;
        return 1;   /* short-circuit walk_dir_file */
    }
    return 0;
}

static int do_dir_ufd(FILE *fp, const ods1_home_t *home,
                      const indexf_t *ix, const Mount *m, int g, int mp) {
    ufd_find_t  finder;
    ufd_state_t s = {0};
    int rc;

    uic_to_ufd_name(g, mp, finder.target);
    finder.found_fnum = 0;
    finder.found_fseq = 0;
    rc = walk_dir_file(fp, home, ix, ODS1_FNUM_MASTER_DIR,
                       find_ufd_cb, &finder);
    if (rc < 0) return -1;
    if (finder.found_fnum == 0) {
        fprintf(stderr, "?UFD [%d,%d] not found in MFD\n", g, mp);
        return -1;
    }

    printf(" Directory %s [%d,%d]\n", m->path, g, mp);
    printf("  Filename        FID\n");
    printf("  --------------  -----------\n");
    if (walk_dir_file(fp, home, ix, finder.found_fnum, ufd_cb, &s) != 0) {
        return -1;
    }
    printf("\n  %d files\n", s.count);
    return 0;
}

/* ----------------------------------------------------------------------
 *  Public entry point (called from cmd_dir.c after kind/sniff dispatch)
 * ---------------------------------------------------------------------- */

int ods1_cmd_dir(Mount *m, const char *uic_arg) {
    ods1_home_t home;
    indexf_t    ix;
    int         g = 0, mp = 0;

    if (!load_home(m->fp, &home)) {
        fprintf(stderr, "?'%s' is not a valid Files-11 ODS-1 volume\n",
                m->path);
        return -1;
    }

    /* Load INDEXF.SYS retrieval cache once.  If it fails we fall back
     * to the naive linear LBN formula (only safe for unfragmented
     * INDEXFs); read_fh handles the fallback automatically. */
    if (load_indexf(m->fp, &home, &ix) != 0) {
        memset(&ix, 0, sizeof(ix));   /* force fallback path */
        fprintf(stderr,
                "  ?Warning: could not load INDEXF.SYS retrieval cache; "
                "lookups for FID > 16 will use the naive LBN formula\n");
    }

    if (uic_arg && uic_arg[0]) {
        if (!parse_uic_arg(uic_arg, &g, &mp)) {
            fprintf(stderr, "?Bad UIC argument '%s' (expected [g,m])\n",
                    uic_arg);
            return -1;
        }
        return do_dir_ufd(m->fp, &home, &ix, m, g, mp);
    }
    return do_dir_mfd(m->fp, &home, &ix, m);
}

/* ----------------------------------------------------------------------
 *  COPY OUT (single file + wildcard)
 * ---------------------------------------------------------------------- */

/* Local glob: case-insensitive shell-style with * and ?. */
static int ods1_glob_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            while (pat[1] == '*') pat++;
            if (pat[1] == '\0') return 1;
            pat++;
            while (*name) {
                if (ods1_glob_match(pat, name)) return 1;
                name++;
            }
            return ods1_glob_match(pat, name);
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

static void ods1_rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n-1] == ' ') s[--n] = '\0';
}

static void ods1_apply_case(char *s, int mode) {
    if (mode == 2) return;
    for (; *s; s++) {
        if (mode == 0 && *s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
        if (mode == 1 && *s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
    }
}

static int ods1_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Find a UFD [g,mp] in the MFD, return its FID. */
static int find_ufd_fid(FILE *fp, const ods1_home_t *home, const indexf_t *ix,
                       int g, int mp, uint16_t *out_fnum) {
    ufd_find_t finder;
    int rc;
    uic_to_ufd_name(g, mp, finder.target);
    finder.found_fnum = 0;
    finder.found_fseq = 0;
    rc = walk_dir_file(fp, home, ix, ODS1_FNUM_MASTER_DIR,
                       find_ufd_cb, &finder);
    if (rc < 0) return -1;
    if (finder.found_fnum == 0) return -1;
    *out_fnum = finder.found_fnum;
    return 0;
}

/* Per-file callback context for copy/wild walks. */
typedef struct {
    /* Inputs */
    FILE             *fp;
    const ods1_home_t *home;
    const indexf_t   *ix;
    const char       *name_pat;
    const char       *type_pat;
    const char       *host_dir;     /* ends with '/' or '\\' */
    int               case_mode;
    /* Single-file mode (when name_pat has no wildcard): match by exact
     * name + type, optionally version (0 = any). */
    int               single_mode;
    int               want_version;
    const char       *single_host_path;
    /* Output */
    int               copied;
    int               skipped;
    int               errors;
    int               found_single;  /* set when single-mode hits */
} copy_ctx_t;

/* Stream the data blocks of a file to an open FILE *.  ods1_walk_map's
 * callback signature has no FILE* slot, so we shuttle the source via
 * a per-call static pointer (g_ods1_copy_fp).  Single-threaded only. */
typedef struct {
    FILE   *out;
    long    written;
    long    cap_blocks;     /* upper bound from EFBK; 0 = whole map */
} stream_ctx_t;

static FILE *g_ods1_copy_fp = NULL;

static void stream_run_cb_real(void *arg, const ods1_retr_t *r, uint32_t idx) {
    stream_ctx_t *s = (stream_ctx_t *)arg;
    uint32_t k;
    (void)idx;
    if (!s->out || !g_ods1_copy_fp) return;
    for (k = 0; k < r->count; k++) {
        uint8_t buf[ODS1_BLOCK_SIZE];
        if (s->cap_blocks > 0 && s->written >= s->cap_blocks) return;
        if (read_block(g_ods1_copy_fp, r->lbn + k, buf) != 0) return;
        if (fwrite(buf, 1, ODS1_BLOCK_SIZE, s->out) != ODS1_BLOCK_SIZE)
            return;
        s->written++;
    }
}

/* Extract a single file given its FID.  Reads the file header, walks
 * its map area, and writes data blocks to host_path.  cap_blocks > 0
 * caps to that many blocks (EFBK from UFAT); 0 = whole map. */
static int extract_fid(FILE *fp, const ods1_home_t *home,
                       const indexf_t *ix, uint16_t fnum,
                       const char *host_path) {
    uint8_t      fh_buf[ODS1_BLOCK_SIZE];
    ods1_fh_t    fh;
    stream_ctx_t sc;
    FILE        *out;
    int          rc;
    if (read_fh(fp, home, ix, fnum, fh_buf, &fh) != 0) {
        fprintf(stderr, "?Cannot read file header for FID=%u\n",
                (unsigned)fnum);
        return -1;
    }
    out = fopen(host_path, "wb");
    if (!out) {
        fprintf(stderr, "?Cannot create '%s'\n", host_path);
        return -1;
    }
    sc.out        = out;
    sc.written    = 0;
    sc.cap_blocks = (long)fh.efbk;       /* EFBK = end-of-file block */
    g_ods1_copy_fp = fp;
    rc = ods1_walk_map(fh_buf, &fh, stream_run_cb_real, &sc);
    g_ods1_copy_fp = NULL;
    fclose(out);
    if (rc < 0) {
        fprintf(stderr, "?Bad map area in FID=%u\n", (unsigned)fnum);
        return -1;
    }
    return (int)sc.written;
}

/* Per-entry callback used by both single-file and wildcard COPY. */
static int copy_dirent_cb(void *arg, const ods1_dirent_t *de) {
    copy_ctx_t *ctx = (copy_ctx_t *)arg;
    char  name_t[10], type_t[4];
    char  host_path[MOUNT_PATH_MAX];
    char  hostname[16];
    int   written;

    if (de->fnum == 0) return 0;

    strlcopy(name_t, de->name, sizeof(name_t));
    strlcopy(type_t, de->type, sizeof(type_t));
    ods1_rstrip(name_t);
    ods1_rstrip(type_t);

    if (ctx->single_mode) {
        /* Exact match (case-insensitive); honor version if specified. */
        const char *want_n = ctx->name_pat ? ctx->name_pat : "";
        const char *want_t = ctx->type_pat ? ctx->type_pat : "";
        if (!ods1_glob_match(want_n, name_t)) return 0;
        if (!ods1_glob_match(want_t, type_t)) return 0;
        if (ctx->want_version != 0 && (int)de->version != ctx->want_version)
            return 0;
        ctx->found_single = 1;
        written = extract_fid(ctx->fp, ctx->home, ctx->ix,
                              de->fnum, ctx->single_host_path);
        if (written < 0) {
            ctx->errors++;
            return 1;   /* short-circuit walk */
        }
        printf("Copied [%s.%s;%d] -> '%s' (%d blocks)\n",
               name_t, type_t, (int)de->version, ctx->single_host_path,
               written);
        ctx->copied++;
        return 1;       /* stop after first match */
    }

    /* Wildcard mode. */
    if (!ods1_glob_match(ctx->name_pat, name_t)) return 0;
    if (ctx->type_pat && *ctx->type_pat &&
        !ods1_glob_match(ctx->type_pat, type_t)) return 0;

    snprintf(hostname, sizeof(hostname), "%s%s%s",
             name_t, type_t[0] ? "." : "", type_t);
    ods1_apply_case(hostname, ctx->case_mode);
    snprintf(host_path, sizeof(host_path), "%s%s",
             ctx->host_dir, hostname);

    if (ods1_file_exists(host_path)) {
        fprintf(stderr, "  %s.%s -> %s  SKIPPED (host exists)\n",
                name_t, type_t, host_path);
        ctx->skipped++;
        return 0;
    }
    written = extract_fid(ctx->fp, ctx->home, ctx->ix,
                          de->fnum, host_path);
    if (written < 0) { ctx->errors++; return 0; }
    printf("  %s.%s;%d -> %s  (%d blocks)\n",
           name_t, type_t, (int)de->version, host_path, written);
    ctx->copied++;
    return 0;
}

int ods1_copy_out(Mount *m, int g, int mp,
                  const char *name, const char *type, int version,
                  const char *host_path) {
    ods1_home_t home;
    indexf_t    ix;
    uint16_t    ufd_fid;
    copy_ctx_t  ctx;

    if (!m || !m->fp || !name || !host_path) return -1;
    if (!load_home(m->fp, &home)) {
        fprintf(stderr, "?'%s' is not Files-11 ODS-1\n", m->path);
        return -1;
    }
    if (load_indexf(m->fp, &home, &ix) != 0) memset(&ix, 0, sizeof(ix));
    if (find_ufd_fid(m->fp, &home, &ix, g, mp, &ufd_fid) != 0) {
        fprintf(stderr, "?UFD [%d,%d] not found in MFD\n", g, mp);
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.fp               = m->fp;
    ctx.home             = &home;
    ctx.ix               = &ix;
    ctx.name_pat         = name;
    ctx.type_pat         = type;
    ctx.single_mode      = 1;
    ctx.want_version     = version;
    ctx.single_host_path = host_path;

    walk_dir_file(m->fp, &home, &ix, ufd_fid, copy_dirent_cb, &ctx);
    if (!ctx.found_single) {
        fprintf(stderr, "?File '%s.%s' not found in [%d,%d]\n",
                name, type ? type : "", g, mp);
        return -1;
    }
    return ctx.errors == 0 ? 0 : -1;
}

int ods1_copy_wild(Mount *m, int g, int mp,
                   const char *name_pat, const char *type_pat,
                   const char *host_dir, int case_mode,
                   int *out_copied, int *out_skipped, int *out_errors) {
    ods1_home_t home;
    indexf_t    ix;
    uint16_t    ufd_fid;
    copy_ctx_t  ctx;
    size_t      len;

    if (!m || !m->fp || !name_pat || !host_dir) return -1;
    if (!load_home(m->fp, &home)) {
        fprintf(stderr, "?'%s' is not Files-11 ODS-1\n", m->path);
        return -1;
    }
    if (load_indexf(m->fp, &home, &ix) != 0) memset(&ix, 0, sizeof(ix));
    if (find_ufd_fid(m->fp, &home, &ix, g, mp, &ufd_fid) != 0) {
        fprintf(stderr, "?UFD [%d,%d] not found in MFD\n", g, mp);
        return -1;
    }
    len = strlen(host_dir);
    if (len == 0 || (host_dir[len-1] != '/' && host_dir[len-1] != '\\')) {
        fprintf(stderr, "?Wildcard COPY destination must end in '/' or '\\\\'\n");
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.fp        = m->fp;
    ctx.home      = &home;
    ctx.ix        = &ix;
    ctx.name_pat  = name_pat;
    ctx.type_pat  = type_pat;
    ctx.host_dir  = host_dir;
    ctx.case_mode = case_mode;
    walk_dir_file(m->fp, &home, &ix, ufd_fid, copy_dirent_cb, &ctx);

    if (out_copied)  *out_copied  = ctx.copied;
    if (out_skipped) *out_skipped = ctx.skipped;
    if (out_errors)  *out_errors  = ctx.errors;
    printf("COPY summary: %d copied, %d skipped, %d errors\n",
           ctx.copied, ctx.skipped, ctx.errors);
    return ctx.errors == 0 ? 0 : -1;
}

/* ======================================================================
 *  ODS-1 COPY IN: append a host file as a new entry in UFD [g,mp]
 *
 *  Steps (each phase is "plan first, write only when !dry_run"):
 *
 *   1. Read host file size, derive block count.
 *   2. Find UFD's FID via MFD walk.
 *   3. Find first empty 16-byte slot in UFD (no UFD-extend support yet).
 *   4. Allocate a new FID via the index file bitmap (LBN iblb..iblb+ibsz-1).
 *   5. Allocate N contiguous blocks via BITMAP.SYS storage bitmap.
 *   6. Build File Header Block with one retrieval pointer.
 *   7. Write file header at LBN = iblb+ibsz+(fnum-1).
 *   8. Stream host data to allocated LBNs.
 *   9. Insert dirent in UFD slot.
 *  10. Update both bitmaps.
 *
 *  Limitations: single contiguous extent only; UFD must have a free
 *  slot; no version-collision detection (callers should check first).
 * ====================================================================== */

#include <sys/stat.h>
#include <time.h>

/* RAD-50 encoder (matches the table used by ods1_rad50_decode_*). */
static uint16_t ods1_rad50_encode_3(const char *s, int len) {
    static const char alpha[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789";
    uint16_t w = 0;
    int      i, idx;
    char     c;
    for (i = 0; i < 3; i++) {
        c = (i < len) ? s[i] : ' ';
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        idx = 0;
        if      (c == ' ')                  idx = 0;
        else if (c >= 'A' && c <= 'Z')      idx = 1 + (c - 'A');
        else if (c == '$')                  idx = 27;
        else if (c == '.')                  idx = 28;
        else if (c == '%')                  idx = 29;
        else if (c >= '0' && c <= '9')      idx = 30 + (c - '0');
        w = (uint16_t)(w * 40u + (unsigned)idx);
    }
    return w;
}

/* Encode a 9-char filename into 3 RAD-50 words.  Pads with spaces. */
static void ods1_encode_fname(const char *name, uint16_t out[3]) {
    char pad[9] = {' ',' ',' ',' ',' ',' ',' ',' ',' '};
    int  nl = (int)strlen(name);
    int  i;
    if (nl > 9) nl = 9;
    for (i = 0; i < nl; i++) pad[i] = name[i];
    out[0] = ods1_rad50_encode_3(pad,     3);
    out[1] = ods1_rad50_encode_3(pad + 3, 3);
    out[2] = ods1_rad50_encode_3(pad + 6, 3);
}

static void le16_write(uint8_t *buf, size_t off, uint16_t v) {
    buf[off]     = (uint8_t)(v & 0xff);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xff);
}

static void ods1_compute_checksum(uint8_t *buf) {
    /* Sum of words 0..254 mod 65536, stored at +510 (CKSM). */
    uint32_t sum = 0;
    int      i;
    for (i = 0; i < 510; i += 2) {
        sum += (uint32_t)le16(buf, (size_t)i);
    }
    le16_write(buf, 510, (uint16_t)(sum & 0xffff));
}

/* "DDMMMYY" plus "HHMMSS" date/time format helpers. */
static void ods1_encode_date(char out[7]) {
    int y, mo, d;
    static const char *mname[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                  "JUL","AUG","SEP","OCT","NOV","DEC"};
    today_ymd(&y, &mo, &d);
    snprintf(out, 8, "%02d%s%02d", d % 100,
             mname[(mo - 1) % 12], y % 100);
    /* snprintf wrote 8 incl NUL; we want exactly 7 chars + no NUL */
}
static void ods1_encode_time(char out[6]) {
    time_t now = time(NULL);
    struct tm lt;
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    struct tm *p = localtime(&now);
    lt = *p;
#endif
    snprintf(out, 7, "%02d%02d%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
}

/* Find first 0 bit in a buffer of `nbytes` bytes.  Returns the bit
 * index (0-based) on success, -1 if none free. */
static long find_free_bit(const uint8_t *buf, size_t nbytes) {
    size_t i;
    int    j;
    for (i = 0; i < nbytes; i++) {
        if (buf[i] == 0xff) continue;
        for (j = 0; j < 8; j++) {
            if (!(buf[i] & (1 << j))) return (long)(i * 8 + j);
        }
    }
    return -1;
}

/* Find a run of `count` consecutive 0 bits starting at any aligned
 * boundary in the buffer.  Returns the starting bit index, or -1. */
static long find_free_run(const uint8_t *buf, size_t nbytes, uint32_t count) {
    size_t   total_bits = nbytes * 8;
    size_t   start = 0, run = 0;
    size_t   i;
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

static void set_bits(uint8_t *buf, long start, uint32_t count) {
    long i;
    for (i = 0; i < (long)count; i++) {
        long b = start + i;
        buf[b >> 3] |= (uint8_t)(1u << (b & 7));
    }
}

int ods1_copy_in(Mount *m, int g, int mp,
                 const char *name, const char *type,
                 const char *host_path, int dry_run) {
    ods1_home_t  home;
    indexf_t     ix;
    uint16_t     ufd_fid;
    uint8_t      bitmap_buf[ODS1_BLOCK_SIZE];
    uint8_t      ufd_blk[ODS1_BLOCK_SIZE];
    uint8_t      fh_buf[ODS1_BLOCK_SIZE];
    ods1_fh_t    ufd_fh, bitmap_fh;
    run_collector_t ufd_runs, bitmap_runs;
    FILE        *in = NULL;
    long long    fsize;
    uint32_t     blocks_needed;
    long         fid_bit = -1;
    uint32_t     new_fid;
    long         data_bit = -1;
    uint32_t     data_lbn = 0;
    uint32_t     fh_lbn;
    uint32_t     ufd_data_lbn = 0;
    int          ufd_slot_byte = -1;
    int          k, b, run_idx;
    struct stat  st;
    uint8_t      hdr[ODS1_BLOCK_SIZE];
    uint16_t     fname_words[3];
    uint16_t     fext_word;

    if (!m || !m->fp || !name || !host_path) return -1;

    /* 0. Sniff and load metadata. */
    if (!load_home(m->fp, &home)) {
        fprintf(stderr, "?'%s' is not Files-11 ODS-1\n", m->path);
        return -1;
    }
    if (load_indexf(m->fp, &home, &ix) != 0) {
        fprintf(stderr, "?Cannot load INDEXF.SYS retrieval cache\n");
        return -1;
    }
    if (find_ufd_fid(m->fp, &home, &ix, g, mp, &ufd_fid) != 0) {
        fprintf(stderr, "?UFD [%d,%d] not found\n", g, mp);
        return -1;
    }

    /* 1. Stat host file. */
    if (stat(host_path, &st) != 0) {
        fprintf(stderr, "?Cannot stat host file '%s'\n", host_path);
        return -1;
    }
    fsize = (long long)st.st_size;
    blocks_needed = (uint32_t)((fsize + 511) / 512);
    if (blocks_needed == 0) blocks_needed = 1;   /* even empty file = 1 blk */

    printf("ODS-1 COPY IN%s:\n", dry_run ? "  [DRY RUN]" : "");
    printf("  Source : %s  (%lld bytes, %u blocks)\n",
           host_path, fsize, (unsigned)blocks_needed);
    printf("  Target : [%d,%d]%s.%s\n", g, mp, name, type ? type : "");

    /* 2. Allocate FID: scan the index file bitmap (iblb..iblb+ibsz-1).
     * For simplicity we look only at the first bitmap block. */
    if (read_block(m->fp, home.iblb, bitmap_buf) != 0) {
        fprintf(stderr, "?Cannot read index file bitmap at LBN=%u\n",
                home.iblb);
        return -1;
    }
    fid_bit = find_free_bit(bitmap_buf, ODS1_BLOCK_SIZE);
    if (fid_bit < 0 || (uint32_t)(fid_bit + 1) > home.fmax) {
        fprintf(stderr, "?No free FID available (fmax=%u)\n",
                (unsigned)home.fmax);
        return -1;
    }
    new_fid = (uint32_t)fid_bit + 1u;
    fh_lbn  = home.iblb + home.ibsz + (uint32_t)(fid_bit);
    printf("  FID    : new=%u  (FH at LBN=%u)\n",
           (unsigned)new_fid, (unsigned)fh_lbn);

    /* 3. Allocate data blocks via BITMAP.SYS storage bitmap.
     * Read its file header (FID 2), walk its data extents, scan for
     * a contiguous run.  For simplicity we look only in the first
     * data block of BITMAP.SYS. */
    if (read_fh(m->fp, &home, &ix, ODS1_FNUM_BITMAP, fh_buf,
                &bitmap_fh) != 0) {
        fprintf(stderr, "?Cannot read BITMAP.SYS file header\n");
        return -1;
    }
    bitmap_runs.n = 0; bitmap_runs.truncated = 0;
    if (ods1_walk_map(fh_buf, &bitmap_fh, collect_run, &bitmap_runs) < 0 ||
        bitmap_runs.n == 0) {
        fprintf(stderr, "?BITMAP.SYS has no data extents\n");
        return -1;
    }
    {
        uint32_t bm_lbn = bitmap_runs.runs[0].lbn;
        if (read_block(m->fp, bm_lbn, bitmap_buf) != 0) {
            fprintf(stderr, "?Cannot read storage bitmap at LBN=%u\n",
                    (unsigned)bm_lbn);
            return -1;
        }
        data_bit = find_free_run(bitmap_buf, ODS1_BLOCK_SIZE, blocks_needed);
        if (data_bit < 0) {
            fprintf(stderr,
                    "?No contiguous run of %u free blocks in first "
                    "BITMAP.SYS block\n", (unsigned)blocks_needed);
            return -1;
        }
        data_lbn = (uint32_t)data_bit;     /* bit i tracks LBN i */
        printf("  Data   : LBN=%u .. %u  (%u blocks, contiguous)\n",
               (unsigned)data_lbn,
               (unsigned)(data_lbn + blocks_needed - 1),
               (unsigned)blocks_needed);
    }

    /* 4. Find an empty UFD slot (entry with fnum=0). */
    if (read_fh(m->fp, &home, &ix, ufd_fid, fh_buf, &ufd_fh) != 0) {
        fprintf(stderr, "?Cannot read UFD's FH (FID=%u)\n",
                (unsigned)ufd_fid);
        return -1;
    }
    ufd_runs.n = 0; ufd_runs.truncated = 0;
    if (ods1_walk_map(fh_buf, &ufd_fh, collect_run, &ufd_runs) < 0) {
        fprintf(stderr, "?Bad map area in UFD\n");
        return -1;
    }
    for (run_idx = 0; run_idx < ufd_runs.n && ufd_slot_byte < 0; run_idx++) {
        uint32_t rlbn  = ufd_runs.runs[run_idx].lbn;
        uint32_t rcnt  = ufd_runs.runs[run_idx].count;
        for (b = 0; b < (int)rcnt && ufd_slot_byte < 0; b++) {
            if (read_block(m->fp, rlbn + b, ufd_blk) != 0) continue;
            for (k = 0; k < ODS1_BLOCK_SIZE; k += ODS1_DIRENT_BYTES) {
                if (le16(ufd_blk, k) == 0) {
                    ufd_slot_byte = k;
                    ufd_data_lbn  = rlbn + b;
                    break;
                }
            }
        }
    }
    if (ufd_slot_byte < 0) {
        fprintf(stderr,
                "?UFD [%d,%d] is full (no empty 16-byte slot); UFD-extend "
                "not yet supported\n", g, mp);
        return -1;
    }
    printf("  UFD slot : LBN=%u  byte_off=%d\n",
           (unsigned)ufd_data_lbn, ufd_slot_byte);

    /* 5. Build the File Header Block. */
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 23;                                  /* IDOF (words) */
    hdr[1] = 46;                                  /* MPOF (words) */
    le16_write(hdr, 2, (uint16_t)new_fid);        /* FNUM */
    le16_write(hdr, 4, 1);                        /* FSEQ = 1 (new) */
    le16_write(hdr, 6, 0x0101);                   /* FLEV */
    le16_write(hdr, 8, (uint16_t)((g << 8) | (mp & 0xff)));   /* FOWN */
    le16_write(hdr, 10, 0xE0E0);                  /* FPRO (system OK) */
    le16_write(hdr, 12, 0);                       /* FCHA */
    /* UFAT: RTYP=1 RATT=0 RSIZ=0 HIBK=blocks_needed EFBK=blocks_needed
     * FFBY = bytes used in last block (= fsize % 512, or 0 if exact). */
    hdr[14] = 1;                                  /* F.RTYP */
    hdr[15] = 0;                                  /* F.RATT */
    le16_write(hdr, 16, 512);                     /* F.RSIZ */
    /* HIBK / EFBK are 32-bit DEC half-swap (hi-half then lo-half, each LE). */
    le16_write(hdr, 18, (uint16_t)((blocks_needed >> 16) & 0xffff));
    le16_write(hdr, 20, (uint16_t)(blocks_needed & 0xffff));
    le16_write(hdr, 22, (uint16_t)((blocks_needed >> 16) & 0xffff));
    le16_write(hdr, 24, (uint16_t)(blocks_needed & 0xffff));
    le16_write(hdr, 26, (uint16_t)(fsize % 512));

    /* Ident area at byte 46. */
    ods1_encode_fname(name, fname_words);
    fext_word = ods1_rad50_encode_3(type ? type : "", type ? (int)strlen(type) : 0);
    le16_write(hdr, 46, fname_words[0]);
    le16_write(hdr, 48, fname_words[1]);
    le16_write(hdr, 50, fname_words[2]);
    le16_write(hdr, 52, fext_word);
    le16_write(hdr, 54, 1);                       /* FVER = 1 */
    le16_write(hdr, 56, 0);                       /* RVNO */
    {
        char  d[8], t[8];
        ods1_encode_date(d);
        ods1_encode_time(t);
        memcpy(hdr + 58, d, 7);                   /* RVDT */
        memcpy(hdr + 65, t, 6);                   /* RVTI */
        memcpy(hdr + 71, d, 7);                   /* CRDT */
        memcpy(hdr + 78, t, 6);                   /* CRTI */
        /* EXDT left zero. */
    }

    /* Map area at byte 92. */
    hdr[92] = 0;                                  /* M.ESQN */
    hdr[93] = 0;                                  /* M.ERVN */
    le16_write(hdr, 94, 0);                       /* M.EFNU */
    le16_write(hdr, 96, 0);                       /* M.EFSQ */
    hdr[98]  = 1;                                 /* M.CTSZ = 1 */
    hdr[99]  = 3;                                 /* M.LBSZ = 3 */
    hdr[100] = 2;                                 /* M.USE = 2 words */
    hdr[101] = 204;                               /* M.MAX */
    /* Single retrieval pointer (format 1: hi-byte, count-1, low-word). */
    hdr[102] = (uint8_t)((data_lbn >> 16) & 0xff);
    hdr[103] = (uint8_t)((blocks_needed - 1) & 0xff);
    le16_write(hdr, 104, (uint16_t)(data_lbn & 0xffff));

    ods1_compute_checksum(hdr);

    /* 6. From here we either dry-run (print summary) or actually write. */
    if (dry_run) {
        printf("\n  [DRY RUN] no writes made to disk image.\n");
        printf("  Would write:\n"
               "    1) BITMAP @LBN=%u  bit %ld set (FID alloc)\n"
               "    2) BITMAP.SYS data block bits %ld..%ld set (data alloc)\n"
               "    3) File header @LBN=%u  (%u-byte block)\n"
               "    4) Data         @LBN=%u..%u (%u blocks)\n"
               "    5) Dirent in UFD@LBN=%u byte=%d\n",
               (unsigned)home.iblb, fid_bit,
               data_bit, data_bit + blocks_needed - 1,
               (unsigned)fh_lbn, (unsigned)ODS1_BLOCK_SIZE,
               (unsigned)data_lbn, (unsigned)(data_lbn + blocks_needed - 1),
               (unsigned)blocks_needed,
               (unsigned)ufd_data_lbn, ufd_slot_byte);
        return 0;
    }

    /* === REAL WRITES from here === */

    /* 7. Open host file for streaming. */
    in = fopen(host_path, "rb");
    if (!in) {
        fprintf(stderr, "?Cannot open host '%s' for reading\n", host_path);
        return -1;
    }

    /* 8. Update index file bitmap. */
    if (read_block(m->fp, home.iblb, bitmap_buf) != 0) {
        fprintf(stderr, "?Re-read index bitmap failed\n");
        fclose(in); return -1;
    }
    set_bits(bitmap_buf, fid_bit, 1);
    if (RT_FSEEK64(m->fp, (long long)home.iblb * 512LL, SEEK_SET) != 0 ||
        fwrite(bitmap_buf, 1, ODS1_BLOCK_SIZE, m->fp) != ODS1_BLOCK_SIZE) {
        fprintf(stderr, "?Index bitmap write failed\n");
        fclose(in); return -1;
    }

    /* 9. Update storage bitmap (BITMAP.SYS first data block). */
    {
        uint32_t bm_lbn = bitmap_runs.runs[0].lbn;
        if (read_block(m->fp, bm_lbn, bitmap_buf) != 0) {
            fprintf(stderr, "?Re-read storage bitmap failed\n");
            fclose(in); return -1;
        }
        set_bits(bitmap_buf, data_bit, blocks_needed);
        if (RT_FSEEK64(m->fp, (long long)bm_lbn * 512LL, SEEK_SET) != 0 ||
            fwrite(bitmap_buf, 1, ODS1_BLOCK_SIZE, m->fp) != ODS1_BLOCK_SIZE) {
            fprintf(stderr, "?Storage bitmap write failed\n");
            fclose(in); return -1;
        }
    }

    /* 10. Write the file header. */
    if (RT_FSEEK64(m->fp, (long long)fh_lbn * 512LL, SEEK_SET) != 0 ||
        fwrite(hdr, 1, ODS1_BLOCK_SIZE, m->fp) != ODS1_BLOCK_SIZE) {
        fprintf(stderr, "?File header write failed\n");
        fclose(in); return -1;
    }

    /* 11. Stream the host data into the allocated extent. */
    {
        uint8_t  buf[ODS1_BLOCK_SIZE];
        uint32_t i;
        for (i = 0; i < blocks_needed; i++) {
            size_t got;
            memset(buf, 0, sizeof(buf));
            got = fread(buf, 1, ODS1_BLOCK_SIZE, in);
            if (RT_FSEEK64(m->fp, (long long)(data_lbn + i) * 512LL,
                           SEEK_SET) != 0 ||
                fwrite(buf, 1, ODS1_BLOCK_SIZE, m->fp) != ODS1_BLOCK_SIZE) {
                fprintf(stderr, "?Data write failed at LBN=%u\n",
                        (unsigned)(data_lbn + i));
                fclose(in); return -1;
            }
            (void)got;
        }
    }
    fclose(in);

    /* 12. Insert the dirent into the UFD slot. */
    if (read_block(m->fp, ufd_data_lbn, ufd_blk) != 0) {
        fprintf(stderr, "?UFD re-read failed\n");
        return -1;
    }
    le16_write(ufd_blk, ufd_slot_byte + 0, (uint16_t)new_fid);   /* fnum */
    le16_write(ufd_blk, ufd_slot_byte + 2, 1);                    /* fseq */
    le16_write(ufd_blk, ufd_slot_byte + 4, 0);                    /* frvn */
    le16_write(ufd_blk, ufd_slot_byte + 6,  fname_words[0]);
    le16_write(ufd_blk, ufd_slot_byte + 8,  fname_words[1]);
    le16_write(ufd_blk, ufd_slot_byte + 10, fname_words[2]);
    le16_write(ufd_blk, ufd_slot_byte + 12, fext_word);
    le16_write(ufd_blk, ufd_slot_byte + 14, 1);                   /* version */
    if (RT_FSEEK64(m->fp, (long long)ufd_data_lbn * 512LL, SEEK_SET) != 0 ||
        fwrite(ufd_blk, 1, ODS1_BLOCK_SIZE, m->fp) != ODS1_BLOCK_SIZE) {
        fprintf(stderr, "?UFD entry write failed\n");
        return -1;
    }

    fflush(m->fp);
    printf("Copied '%s' -> A:[%d,%d]%s.%s;1  "
           "(FID=%u, %u blocks, LBN=%u)\n",
           host_path, g, mp, name, type ? type : "",
           (unsigned)new_fid, (unsigned)blocks_needed, (unsigned)data_lbn);
    return 0;
}
