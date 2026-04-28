/*
 * mount.h - In-memory mount table for virtual disks (DV) and tapes (MT).
 *
 * Each mount can be:
 *   - Opened from a file path (MOUNT fn[.dsk|.tap]).
 *   - Optionally tagged with a drive letter via ASSGN, so the rest of
 *     the commands can refer to it as "A:", "B:", etc. (case-insensitive).
 *
 * The kind of device is selected by the file extension at MOUNT time:
 *   *.dsk, *.rk, *.rl, (default) -> MOUNT_KIND_DV  (RT-11 / TSX+ disk)
 *   *.tap, *.mt                  -> MOUNT_KIND_MT  (SIMH .tap container)
 *   *.tar                        -> MOUNT_KIND_TAR (POSIX ustar archive,
 *                                                   read-only listing only)
 *
 * Every file keeps its FILE* open in "r+b" mode for read/write access
 * (TAR falls back to "rb" if the user has no write permission).
 */
#ifndef RT11DV_MOUNT_H
#define RT11DV_MOUNT_H

#include <stdint.h>
#include <stdio.h>

#include "rt11.h"

#define MOUNT_MAX       26     /* one per drive letter */
#define MOUNT_PATH_MAX  520    /* Windows path max-ish */

typedef enum {
    MOUNT_KIND_DV  = 0,    /* RT-11 / TSX+ random-access disk (.dsk) */
    MOUNT_KIND_MT  = 1,    /* RT-11 FSM sequential magtape (.tap)    */
    MOUNT_KIND_TAR = 2     /* POSIX ustar archive (.tar)             */
} MountKind;

typedef struct {
    int       used;                 /* 1 if entry is alive */
    char      letter;               /* assigned drive letter (0 if none) */
    MountKind kind;                 /* DV or MT */
    char      path[MOUNT_PATH_MAX]; /* absolute/relative path to the image */
    FILE     *fp;                   /* open handle (r+b) */
    uint32_t  total_blocks;         /* DV: size of the image in 512-byte blocks
                                     * MT: not used                          */
    long long size_bytes;           /* total file size in bytes (both kinds) */

    /* Working-directory state for hierarchical filesystems (RSTS/E and
     * ODS-1).  cwd_g == 0 && cwd_p == 0 means "no cwd set" (root/MFD).
     * Set by `CD A:[g,m]`, cleared by `CD A:` or `CD ..`.  RT-11, MT
     * and TAR mounts ignore these fields. */
    int       cwd_g;
    int       cwd_p;
} Mount;

/* Initialise the global mount table. */
void mount_init(void);

/* Free all mounts and close their handles. */
void mount_shutdown(void);

/* Open/mount a DV file. Returns pointer to the Mount slot on success or
 * NULL on failure (reason printed to stderr). If a mount for the same
 * path already exists, that one is returned. */
Mount *mount_open(const char *path);

/* Dismount a DV. NULL-safe. */
void mount_close(Mount *m);

/* Find an already-mounted DV by path (case-insensitive on Windows). */
Mount *mount_find_by_path(const char *path);

/* Find an already-mounted DV by drive letter ('A'..'Z', case-insensitive). */
Mount *mount_find_by_letter(char letter);

/* Return the first mount of the requested kind, or NULL if none. Handy
 * for MT: which is conceptually the single tape drive. */
Mount *mount_first_by_kind(MountKind kind);

/* Assign a drive letter to a mounted DV. If another DV already owns that
 * letter, the assignment is transferred. Returns 0 on success, -1 if the
 * letter is invalid. */
int mount_assign_letter(Mount *m, char letter);

/* List all active mounts on stdout (for the DIR-alone / status case). */
void mount_list(void);

#endif /* RT11DV_MOUNT_H */
