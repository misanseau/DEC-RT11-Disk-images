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
 * mount.c - In-memory mount table (single process, single session).
 */
#include "mount.h"
#include "util.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Mount g_mounts[MOUNT_MAX];

void mount_init(void) {
    memset(g_mounts, 0, sizeof(g_mounts));
}

void mount_shutdown(void) {
    int i;
    for (i = 0; i < MOUNT_MAX; i++) {
        if (g_mounts[i].used && g_mounts[i].fp) {
            fclose(g_mounts[i].fp);
            g_mounts[i].fp = NULL;
        }
        g_mounts[i].used = 0;
    }
}

Mount *mount_find_by_path(const char *path) {
    int i;
    if (!path) return NULL;
    for (i = 0; i < MOUNT_MAX; i++) {
        if (g_mounts[i].used && strcieq(g_mounts[i].path, path)) {
            return &g_mounts[i];
        }
    }
    return NULL;
}

Mount *mount_first_by_kind(MountKind kind) {
    int i;
    for (i = 0; i < MOUNT_MAX; i++) {
        if (g_mounts[i].used && g_mounts[i].kind == kind) {
            return &g_mounts[i];
        }
    }
    return NULL;
}

Mount *mount_find_by_letter(char letter) {
    int i;
    letter = (char)toupper((unsigned char)letter);
    if (letter < 'A' || letter > 'Z') return NULL;
    for (i = 0; i < MOUNT_MAX; i++) {
        if (g_mounts[i].used && g_mounts[i].letter == letter) {
            return &g_mounts[i];
        }
    }
    return NULL;
}

static MountKind kind_from_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return MOUNT_KIND_DV;
    if (strcieq(dot, ".tap") || strcieq(dot, ".mt"))  return MOUNT_KIND_MT;
    if (strcieq(dot, ".tar"))                         return MOUNT_KIND_TAR;
    return MOUNT_KIND_DV;
}

Mount *mount_open(const char *path) {
    int i;
    Mount *slot;
    FILE *fp;
    Mount *existing;
    MountKind kind;
    long long sz;

    if (!path || !*path) return NULL;

    existing = mount_find_by_path(path);
    if (existing) {
        return existing;
    }

    /* Find a free slot. */
    slot = NULL;
    for (i = 0; i < MOUNT_MAX; i++) {
        if (!g_mounts[i].used) {
            slot = &g_mounts[i];
            break;
        }
    }
    if (!slot) {
        fprintf(stderr, "?Mount table full (max %d)\n", MOUNT_MAX);
        return NULL;
    }

    kind = kind_from_path(path);

    fp = fopen(path, "r+b");
    if (!fp) {
        /* For tapes we allow creating an empty file on first mount so that
         * FORMAT MT: can initialise it.  DV files still have to exist.
         * TAR archives are read-only for now and must already exist.    */
        if (kind == MOUNT_KIND_MT) {
            fp = fopen(path, "w+b");
            if (!fp) {
                fprintf(stderr, "?Cannot create tape image '%s'\n", path);
                return NULL;
            }
        } else if (kind == MOUNT_KIND_TAR) {
            /* Try read-only fallback so the user can DIR pre-existing
             * tarballs that they don't have write permission to.        */
            fp = fopen(path, "rb");
            if (!fp) {
                fprintf(stderr, "?Cannot open tar archive '%s'\n", path);
                return NULL;
            }
        } else {
            fprintf(stderr, "?Cannot open '%s' for read/write\n", path);
            return NULL;
        }
    }

    /* Measure the file size.  For DV we convert to 512-byte blocks;
     * for MT we just remember the raw byte size. */
    if (RT_FSEEK64(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "?Cannot seek to end of '%s'\n", path);
        fclose(fp);
        return NULL;
    }
    sz = RT_FTELL64(fp);

    if (kind == MOUNT_KIND_DV) {
        if (sz < (long long)(RT11_DIR_START_BLOCK + 2) * RT11_BLOCK_SIZE) {
            fprintf(stderr, "?'%s' is too small to be an RT-11 volume\n", path);
            fclose(fp);
            return NULL;
        }
        slot->total_blocks = (uint32_t)(sz / RT11_BLOCK_SIZE);
    } else if (kind == MOUNT_KIND_TAR) {
        /* Tar archives must be a multiple of 512 to be well-formed but
         * we leave validation to the decoder so the user can still DIR
         * a slightly truncated file and see what's there.              */
        slot->total_blocks = (uint32_t)(sz / 512);
    } else {
        slot->total_blocks = 0;  /* MT: sequential, block count N/A */
    }
    slot->size_bytes = sz;

    if (RT_FSEEK64(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "?Cannot rewind '%s'\n", path);
        fclose(fp);
        return NULL;
    }

    slot->used   = 1;
    slot->letter = 0;
    slot->kind   = kind;
    slot->fp     = fp;
    slot->cwd_g  = 0;
    slot->cwd_p  = 0;
    strlcopy(slot->path, path, sizeof(slot->path));
    return slot;
}

void mount_close(Mount *m) {
    if (!m) return;
    if (m->fp) {
        fclose(m->fp);
        m->fp = NULL;
    }
    m->used         = 0;
    m->letter       = 0;
    m->kind         = MOUNT_KIND_DV;
    m->total_blocks = 0;
    m->size_bytes   = 0;
    m->cwd_g        = 0;
    m->cwd_p        = 0;
    m->path[0]      = '\0';
}

int mount_assign_letter(Mount *m, char letter) {
    int i;
    letter = (char)toupper((unsigned char)letter);
    if (letter < 'A' || letter > 'Z') return -1;
    if (!m || !m->used) return -1;

    /* Clear the same letter from any other mount. */
    for (i = 0; i < MOUNT_MAX; i++) {
        if (g_mounts[i].used && &g_mounts[i] != m &&
            g_mounts[i].letter == letter) {
            g_mounts[i].letter = 0;
        }
    }
    m->letter = letter;
    return 0;
}

void mount_list(void) {
    int i, any = 0;
    printf("LIST - mounted volumes:\n");
    for (i = 0; i < MOUNT_MAX; i++) {
        if (g_mounts[i].used) {
            const char *kind_name;
            char size_text[24];
            if (!any) {
                printf("  Letter  Kind    Size       File\n");
                printf("  ------  ----  --------  --------------------------------\n");
                any = 1;
            }
            switch (g_mounts[i].kind) {
                case MOUNT_KIND_MT:  kind_name = " MT ";  break;
                case MOUNT_KIND_TAR: kind_name = " TAR";  break;
                default:             kind_name = " DV ";  break;
            }
            if (g_mounts[i].kind == MOUNT_KIND_MT) {
                snprintf(size_text, sizeof(size_text), "%lld B",
                         g_mounts[i].size_bytes);
            } else {
                snprintf(size_text, sizeof(size_text), "%u blk",
                         (unsigned)g_mounts[i].total_blocks);
            }
            if (g_mounts[i].letter) {
                printf("    %c:   %s  %-8s  %s\n",
                       g_mounts[i].letter, kind_name, size_text,
                       g_mounts[i].path);
            } else {
                printf("    -    %s  %-8s  %s\n",
                       kind_name, size_text, g_mounts[i].path);
            }
        }
    }
    if (!any) {
        printf("  (no virtual volumes mounted)\n");
    }
}
