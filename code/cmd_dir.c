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
 * cmd_dir.c - DIR command implementation.
 *
 * DIR can do three things, decided by the argument:
 *
 *   DIR                      -> list the current host (Windows/POSIX)
 *                               directory.
 *   DIR <host-path>          -> stat or list a host file or directory.
 *   DIR <letter>: | MT:      -> dispatch to the volume-specific DIR
 *                               (rt11_cmd_dir / mt_cmd_dir / ... ).
 *
 * Host listing has Win32 and POSIX backends; both produce the same
 * "Name | Modified | Size" three-column format used by the rest of
 * the tool.
 */
#include "cmd_internal.h"
#include "commands.h"
#include "mount.h"
#include "mt.h"
#include "ods1.h"
#include "rsts.h"
#include "rt11.h"
#include "tar.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#if defined(_WIN32) || defined(_MSC_VER)
#  include <windows.h>
#  include <direct.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

/* ----------------------------------------------------------------------
 *   Pretty-printing helpers (size + mtime)
 * ---------------------------------------------------------------------- */

static void fmt_size(long long sz, char *out, size_t n) {
    if (sz < 0)              snprintf(out, n, "       -");
    else if (sz < 1024)      snprintf(out, n, "%5lld B ", sz);
    else if (sz < 1024*1024) snprintf(out, n, "%5.1f KB", sz / 1024.0);
    else                     snprintf(out, n, "%5.1f MB", sz / (1024.0 * 1024.0));
}

static void fmt_mtime(time_t mt, char *out, size_t n) {
    struct tm *tm = localtime(&mt);
    if (!tm) { snprintf(out, n, "                    "); return; }
    strftime(out, n, "%Y-%m-%d %H:%M", tm);
}

/* ----------------------------------------------------------------------
 *   Host-side directory + stat (Windows / POSIX)
 * ---------------------------------------------------------------------- */

#if defined(_WIN32) || defined(_MSC_VER)

static int host_list_dir_win(const char *path) {
    char pattern[MOUNT_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    size_t plen = strlen(path);
    int has_sep = (plen > 0 && (path[plen-1] == '\\' || path[plen-1] == '/'));
    long long total = 0;
    int count = 0;

    snprintf(pattern, sizeof(pattern), "%s%s*", path, has_sep ? "" : "\\");

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "?Cannot list host directory '%s'\n", path);
        return -1;
    }

    printf(" Directory of %s\n\n", path);
    printf("  %-40s %-16s %10s\n", "Name", "Modified", "Size");
    printf("  ---------------------------------------- ----------------  ----------\n");
    do {
        char sizebuf[16];
        char timebuf[24];
        ULARGE_INTEGER size;
        FILETIME ft;
        SYSTEMTIME st_local;
        struct tm tmv = {0};
        time_t mt;

        if (strcmp(fd.cFileName, ".") == 0) continue;

        size.HighPart = fd.nFileSizeHigh;
        size.LowPart  = fd.nFileSizeLow;

        /* Convert FILETIME -> time_t */
        ft = fd.ftLastWriteTime;
        if (FileTimeToLocalFileTime(&ft, &ft) &&
            FileTimeToSystemTime(&ft, &st_local)) {
            tmv.tm_year = st_local.wYear - 1900;
            tmv.tm_mon  = st_local.wMonth - 1;
            tmv.tm_mday = st_local.wDay;
            tmv.tm_hour = st_local.wHour;
            tmv.tm_min  = st_local.wMinute;
            tmv.tm_sec  = st_local.wSecond;
            mt = mktime(&tmv);
        } else {
            mt = 0;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            snprintf(sizebuf, sizeof(sizebuf), "   <DIR> ");
        } else {
            fmt_size((long long)size.QuadPart, sizebuf, sizeof(sizebuf));
            total += (long long)size.QuadPart;
        }
        fmt_mtime(mt, timebuf, sizeof(timebuf));
        printf("  %-40s %-16s  %s\n", fd.cFileName, timebuf, sizebuf);
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("\n  %d entries, %lld bytes total\n", count, total);
    return 0;
}

static int host_stat(const char *path, int *is_dir, long long *size,
                     time_t *mtime) {
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return -1;
    *is_dir = (st.st_mode & _S_IFDIR) ? 1 : 0;
    *size   = (long long)st.st_size;
    *mtime  = (time_t)st.st_mtime;
    return 0;
}

#else   /* POSIX */

static int host_list_dir_posix(const char *path) {
    DIR *d;
    struct dirent *e;
    long long total = 0;
    int count = 0;
    d = opendir(path);
    if (!d) {
        fprintf(stderr, "?Cannot list host directory '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    printf(" Directory of %s\n\n", path);
    printf("  %-40s %-16s %10s\n", "Name", "Modified", "Size");
    printf("  ---------------------------------------- ----------------  ----------\n");
    while ((e = readdir(d)) != NULL) {
        char full[MOUNT_PATH_MAX];
        struct stat st;
        char sizebuf[16];
        char timebuf[24];
        int is_dir;
        if (strcmp(e->d_name, ".") == 0) continue;
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (stat(full, &st) != 0) continue;
        is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        if (is_dir) {
            snprintf(sizebuf, sizeof(sizebuf), "   <DIR> ");
        } else {
            fmt_size((long long)st.st_size, sizebuf, sizeof(sizebuf));
            total += (long long)st.st_size;
        }
        fmt_mtime(st.st_mtime, timebuf, sizeof(timebuf));
        printf("  %-40s %-16s  %s\n", e->d_name, timebuf, sizebuf);
        count++;
    }
    closedir(d);
    printf("\n  %d entries, %lld bytes total\n", count, total);
    return 0;
}

static int host_stat(const char *path, int *is_dir, long long *size,
                     time_t *mtime) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    *size   = (long long)st.st_size;
    *mtime  = st.st_mtime;
    return 0;
}

#endif

static int host_list_path(const char *path) {
    int is_dir;
    long long size;
    time_t mtime;
    char sizebuf[16], timebuf[24];

    if (host_stat(path, &is_dir, &size, &mtime) != 0) {
        fprintf(stderr, "?Host path not found: '%s'\n", path);
        return -1;
    }
    if (is_dir) {
#if defined(_WIN32) || defined(_MSC_VER)
        return host_list_dir_win(path);
#else
        return host_list_dir_posix(path);
#endif
    }
    /* Single-file listing. */
    fmt_size(size, sizebuf, sizeof(sizebuf));
    fmt_mtime(mtime, timebuf, sizeof(timebuf));
    printf(" File: %s\n", path);
    printf("  Modified: %s\n", timebuf);
    printf("  Size    : %s (%lld bytes)\n", sizebuf, size);
    return 0;
}

/* ----------------------------------------------------------------------
 *   Device-reference helpers
 * ---------------------------------------------------------------------- */

/* Return 1 iff `arg` starts with a recognised device prefix:
 *   "A:" .. "Z:" (single-letter drive) or "MT:" (tape). */
static int looks_like_device(const char *arg) {
    if (!arg || !arg[0]) return 0;
    if (arg[0] && arg[1] == ':' && isalpha((unsigned char)arg[0])) {
        /* reject "C:\foo" style Windows absolute path */
        if (arg[2] == '\\' || arg[2] == '/') return 0;
        return 1;
    }
    if ((arg[0] == 'M' || arg[0] == 'm') &&
        (arg[1] == 'T' || arg[1] == 't') &&
        arg[2] == ':')
        return 1;
    return 0;
}

/* find_tape() comes from cmd_internal.h (defined in commands.c). */

/* ----------------------------------------------------------------------
 *   DIR command
 * ---------------------------------------------------------------------- */

int do_dir(char *tokens[], int n) {
    int i = 1;
    Mount *m;
    Rt11Dev dev;

    if (i >= n) {
        /* DIR with no argument -> list the current host directory. */
        return host_list_path(".");
    }

    /* With argument: decide between device ref and host path. */
    if (!looks_like_device(tokens[i])) {
        /* Host path / filename (NO auto-mount of .dsk any more; to open
         * a DV image and list it, use MOUNT + DIR letter:). */
        const char *path = tokens[i++];
        if (i != n) {
            fprintf(stderr, "?Extra arguments after DIR\n");
            return -1;
        }
        return host_list_path(path);
    }

    /* MT: tape reference */
    if ((tokens[i][0] == 'M' || tokens[i][0] == 'm') &&
        (tokens[i][1] == 'T' || tokens[i][1] == 't') &&
        tokens[i][2] == ':')
    {
        m = find_tape();
        if (!m) {
            fprintf(stderr, "?No tape is mounted (use MOUNT fn.tap first)\n");
            return -1;
        }
        i++;
        if (i != n) {
            fprintf(stderr, "?Extra arguments after DIR MT:\n");
            return -1;
        }
        return mt_cmd_dir(m);
    }

    /* Single-letter drive */
    m = resolve_dv(tokens, n, &i);
    if (!m) return -1;

    /* Optional UIC argument for ODS-1 / RSTS:  DIR A:[g,m]
     * Captured here before the "no extra args" check below.  If no
     * explicit [g,m] is given but the mount has a cwd set (via CD),
     * synthesise one from the cwd so DIR A: drills into the cwd UFD
     * just like DIR A:[cwd_g,cwd_p] would. */
    {
        const char *uic_arg = NULL;
        char        cwd_arg[20];
        if (i < n && tokens[i][0] == '[') {
            uic_arg = tokens[i++];
        } else if (m->cwd_g != 0 || m->cwd_p != 0) {
            snprintf(cwd_arg, sizeof(cwd_arg), "[%d,%d]",
                     m->cwd_g, m->cwd_p);
            uic_arg = cwd_arg;
        }
        if (i != n) {
            fprintf(stderr, "?Extra arguments after DIR\n");
            return -1;
        }
        if (m->kind == MOUNT_KIND_MT)  return mt_cmd_dir(m);
        if (m->kind == MOUNT_KIND_TAR) return tar_cmd_dir(m);

        /* DV: sniff first ODS-1, then RSTS/E; fall through to RT-11. */
        {
            uint8_t blk1[512];
            ods1_test_t t;
            if (RT_FSEEK64(m->fp, 512LL, SEEK_SET) == 0 &&
                fread(blk1, 1, 512, m->fp) == 512) {
                t = ods1_test(2, m->total_blocks, blk1);
                if (t.result == ODS1_TEST_PASS) {
                    return ods1_cmd_dir(m, uic_arg);
                }
            }
            if (rsts_test(m->fp, m->total_blocks)) {
                return rsts_cmd_dir(m, uic_arg);
            }
            if (uic_arg) {
                fprintf(stderr,
                        "?UIC syntax [%s] is only valid for ODS-1 volumes\n",
                        uic_arg + 1);
                return -1;
            }
        }
        dev.fp           = m->fp;
        dev.path         = m->path;
        dev.total_blocks = m->total_blocks;
        return rt11_cmd_dir(&dev);
    }
}
