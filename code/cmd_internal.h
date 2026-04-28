/*
 * cmd_internal.h - Helpers shared between cmd_*.c command modules and
 *                  the dispatcher in commands.c.  NOT part of the
 *                  public REPL API (that lives in commands.h).
 *
 * Each cmd_*.c implements a single verb (or family of verbs) and
 * exposes a do_<verb>() entry point that the dispatcher calls.  Shared
 * services -- DV resolution, modifier parsing, etc. -- live in
 * commands.c and are re-exported here.
 */
#ifndef RT11DV_CMD_INTERNAL_H
#define RT11DV_CMD_INTERNAL_H

#include "mount.h"

/* Maximum tokens parsed off one command line.  Sub-modules don't need
 * to know this for parsing (the dispatcher already split the line) but
 * they do declare local arrays sized to it occasionally. */
#define MAX_TOKENS 16

/* ---- DV / modifier services exported by commands.c ----------------- */

/* Modifier flag bits (used internally; not all sub-modules need them). */
#define MOD_DL    (1u << 0)
#define MOD_RT11  (1u << 1)
#define MOD_UC    (1u << 2)
#define MOD_LC    (1u << 3)
#define MOD_NC    (1u << 4)
#define MOD_DR    (1u << 5)   /* /DR = dry-run (preview only, no writes) */

typedef struct {
    unsigned seen;   /* bitmap of MOD_* flags that were seen */
} ModFlags;

/* Resolve a "dv" token at tokens[*idx].  It can be either a drive
 * letter ("A:") or a filename (auto-mounted on first reference).
 * Optionally consumes /DL and /RT11 modifiers that follow.  Returns
 * the Mount* on success, NULL on error (after printing to stderr).
 * On success, *idx is advanced past the token + modifiers.            */
Mount *resolve_dv(char *tokens[], int n, int *idx);

/* Return the currently-mounted tape (MT: or TAR), or NULL if none. */
Mount *find_tape(void);

/* ---- Sub-module entry points called by the dispatcher -------------- */

int do_dir(char *tokens[], int n);

/* ---- ODS-1 sub-module entry point --------------------------------- */
/* uic_arg may be NULL (list MFD) or "[g,m]" (list one UFD). */
int ods1_cmd_dir(Mount *m, const char *uic_arg);

/* Extract one file from an ODS-1 volume.  Locates the file by name +
 * type under UFD [g,m]; if `version` is 0, picks the highest version
 * found.  Writes the file's data blocks (per its file-header map area)
 * to host_path. */
int ods1_copy_out(Mount *m, int g, int mp,
                  const char *name, const char *type, int version,
                  const char *host_path);

/* Wildcard variant: glob-match name + type in UFD [g,m], extract each
 * match into host_dir.  case_mode: 0=UC 1=LC 2=NC. */
int ods1_copy_wild(Mount *m, int g, int mp,
                   const char *name_pat, const char *type_pat,
                   const char *host_dir, int case_mode,
                   int *out_copied, int *out_skipped, int *out_errors);

/* Append a host file as a new entry to UFD [g,mp].  Allocates a fresh
 * FID via the index-file bitmap, allocates N contiguous data blocks
 * via BITMAP.SYS, builds a File Header Block, writes the file data,
 * and inserts a new dirent into the UFD.  If `dry_run` is non-zero
 * the function prints the plan and bitmap selections but does NOT
 * write anything to the disk image. */
int ods1_copy_in(Mount *m, int g, int mp,
                 const char *name, const char *type,
                 const char *host_path, int dry_run);

/* ---- RSTS/E sub-module entry point -------------------------------- */
/* uic_arg may be NULL (full pack walk) or "[g,p]" (one UFD only). */
int rsts_cmd_dir(Mount *m, const char *uic_arg);

#endif /* RT11DV_CMD_INTERNAL_H */
