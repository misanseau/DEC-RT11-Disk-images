/*
 * commands.c - Parse a command line, identify the verb, and invoke the
 * appropriate rt11 / mount routine.  Keeps REPL and one-shot CLI mode
 * on exactly the same code path.
 *
 * Grammar (case-insensitive):
 *
 *   CREATE | C  <filename[.dsk]> [/DL] [/RT11] [<letter>:]     [#comment]
 *   FORMAT | F  <dv> [/RT11]                                   [#comment]
 *       /RT11 on FORMAT = only (re)write the 6 boot/home blocks,
 *                         preserving the RT-11 directory area.
 *   BOOT   | B  <dv> <monitor> <handler>                       [#comment]
 *   DIR         [<path>|<dv>|MT:]                              [#comment]
 *       No arg = list the host current directory; <path> = list
 *       a host file/folder; <dv> = list an RT-11 / TSX+ volume
 *       (or, if the slot was MOUNTed from a .tar archive, the
 *       ustar table-of-contents); MT: = list the active tape.
 *   MOUNT  | M  <filename[.dsk]> [/DL] [/RT11] [<letter>:]     [#comment]
 *   ASSGN  | A  <letter>: <filename[.dsk]> [/DL] [/RT11]       [#comment]
 *   COPY   | CP <src> <dst> [/RT11] [/UC|/LC|/NC]              [#comment]
 *       <src>  ::= [<dev:>]<fn>      <fn> ::= <name>[<tipo>] | *[<tipo>] | *.*
 *       <dst>  ::= [<dev:>]<fn>
 *       <dev>  ::= letter previously assigned | MT: (tape)
 *       '*', '*.*', '*.EXT', 'NAME.*' act as file-matching
 *       wildcards; only valid on the <src> side.
 *       /UC = destination filename in UPPERCASE (default)
 *       /LC = destination filename in lowercase
 *       /NC = destination filename kept as-is
 *       Device-to-device copies are NOT supported.
 *   EXAM   | E  [<dv>:]<blk>                                   [#comment]
 *       Dump a single block of a mounted DV: octal words +
 *       ASCII, plus interpretation for known RT-11 blocks
 *       (0 = primary boot, 1 = home block, 2-5 = secondary boot,
 *       6+ = directory segment).
 *   UMOUNT | U  <dv>                                           [#comment]
 *   LIST   | L                                                 [#comment]
 *   VER    | V                       // print version and build timestamp
 *   HELP   | ?
 *   EXIT   | QUIT | Q
 *
 * Any command line may end in "  >> output-file" to append all standard
 * output produced by that command to a host file (errors still go to
 * stderr).
 *
 * Comments: '#' marks the start of an inline comment (rest of the line
 * is ignored).  '#' is only recognised when it appears at the start of a
 * line or is preceded by whitespace, so filenames containing '#' are
 * still handled correctly.
 *
 * A "dv" argument is either a drive letter (e.g. A:) or a mounted DV
 * filename (with optional /DL and /RT11 modifiers).  When a filename
 * has no extension, ".dsk" is assumed.
 */
#include "commands.h"
#include "cmd_internal.h"
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
#if defined(_WIN32) || defined(_MSC_VER)
#  include <io.h>     /* _dup, _dup2, _fileno, _close */
#else
#  include <unistd.h> /* dup, dup2, close          */
#endif
#include <time.h>

#if defined(_WIN32) || defined(_MSC_VER)
#  include <windows.h>
#  include <direct.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

#if defined(_MSC_VER)
#  include <io.h>
#  define RT_DUP    _dup
#  define RT_DUP2   _dup2
#  define RT_CLOSE  _close
#  define RT_FILENO _fileno
#else
#  include <unistd.h>
#  define RT_DUP    dup
#  define RT_DUP2   dup2
#  define RT_CLOSE  close
#  define RT_FILENO fileno
#endif

#define RT11DV_VERSION "0.4"

/* Compile-time platform / build-type detection. */
static const char *rt11dv_platform(void) {
#if defined(_WIN64)
    return "x64";
#elif defined(_WIN32) || defined(_M_IX86)
    return "x32";
#elif defined(__x86_64__) || defined(__aarch64__) || defined(__LP64__)
    return "x64";
#elif defined(__i386__) || defined(__arm__)
    return "x32";
#else
    return "?? ";
#endif
}

static const char *rt11dv_buildtype(void) {
#if defined(_DEBUG) || (defined(DEBUG) && !defined(NDEBUG))
    return "deb";
#else
    return "rel";
#endif
}

/* ----------------------------------------------------------------------
 *   Token helpers
 * ---------------------------------------------------------------------- */
/* MAX_TOKENS comes from cmd_internal.h (shared with sub-modules). */

static int split_tokens(char *line, char *tokens[], int max) {
    int n = 0;
    char *p = line;

    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        tokens[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    return n;
}

/* ----------------------------------------------------------------------
 *   DV resolution
 * ---------------------------------------------------------------------- */

static int looks_like_letter(const char *tok) {
    return tok && isalpha((unsigned char)tok[0]) && tok[1] == ':' && tok[2] == 0;
}

/* Ensure the filename has an extension; appends ".DSK" if needed.
 * Writes up to (MOUNT_PATH_MAX) bytes into out. */
static void ensure_default_ext(const char *fn, char *out, size_t sz) {
    const char *dot = strrchr(fn, '.');
    if (dot && dot != fn) {
        strlcopy(out, fn, sz);
    } else {
        snprintf(out, sz, "%s.dsk", fn);
    }
}

/* MOD_* and ModFlags come from cmd_internal.h.                          */

/* Consume /XXX modifiers starting at tokens[*idx].  `allowed` is a bitmap
 * of MOD_* values - modifiers outside that set are rejected.  On
 * success, out_flags->seen receives the bitmap of flags that were
 * actually present, and *idx is advanced past them.  Returns 0 on
 * success, -1 on bad or duplicate modifier. */
static int consume_modifiers_ex(char *tokens[], int n, int *idx,
                                unsigned allowed, ModFlags *out_flags) {
    int i = *idx;
    unsigned seen = 0;

    while (i < n && tokens[i][0] == '/') {
        unsigned bit = 0;
        const char *tok = tokens[i];
        if      (strcieq(tok, "/DL"))   bit = MOD_DL;
        else if (strcieq(tok, "/RT11")) bit = MOD_RT11;
        else if (strcieq(tok, "/UC"))   bit = MOD_UC;
        else if (strcieq(tok, "/LC"))   bit = MOD_LC;
        else if (strcieq(tok, "/NC"))   bit = MOD_NC;
        else if (strcieq(tok, "/DR"))   bit = MOD_DR;
        else {
            fprintf(stderr, "?Unknown modifier '%s'\n", tok);
            return -1;
        }
        if (!(bit & allowed)) {
            fprintf(stderr, "?Modifier '%s' not allowed here\n", tok);
            return -1;
        }
        if (seen & bit) {
            fprintf(stderr, "?Duplicate modifier '%s'\n", tok);
            return -1;
        }
        /* /UC, /LC and /NC are mutually exclusive */
        if ((bit & (MOD_UC|MOD_LC|MOD_NC)) &&
            (seen & (MOD_UC|MOD_LC|MOD_NC))) {
            fprintf(stderr, "?/UC, /LC and /NC are mutually exclusive\n");
            return -1;
        }
        seen |= bit;
        i++;
    }
    *idx = i;
    if (out_flags) out_flags->seen = seen;
    return 0;
}

/* Back-compat wrapper for callers that just wanted /DL and /RT11. */
static int consume_modifiers(char *tokens[], int n, int *idx) {
    ModFlags f;
    return consume_modifiers_ex(tokens, n, idx, MOD_DL | MOD_RT11, &f);
}

/* Find the currently-mounted tape (MT or TAR), if any.  Exported via
 * cmd_internal.h so the dispatcher and sub-modules can both use it. */
Mount *find_tape(void) {
    return mount_first_by_kind(MOUNT_KIND_MT);
}

/* Given a token that is either "A:" or a filename, return the mount.
 * If a filename and it's not mounted yet, auto-mount it. Optionally
 * consume /DL and /RT11 modifiers that follow. Advances *idx.
 * Exported via cmd_internal.h for use by cmd_*.c sub-modules. */
Mount *resolve_dv(char *tokens[], int n, int *idx) {
    char path[MOUNT_PATH_MAX];
    Mount *m;
    int i = *idx;

    if (i >= n) {
        fprintf(stderr, "?Missing DV argument\n");
        return NULL;
    }

    if (looks_like_letter(tokens[i])) {
        m = mount_find_by_letter(tokens[i][0]);
        if (!m) {
            fprintf(stderr, "?Drive letter %c: not assigned\n",
                    (char)toupper((unsigned char)tokens[i][0]));
            return NULL;
        }
        i++;
        if (consume_modifiers(tokens, n, &i) != 0) return NULL;
        *idx = i;
        return m;
    }

    /* Otherwise it's a filename. */
    ensure_default_ext(tokens[i], path, sizeof(path));
    i++;
    if (consume_modifiers(tokens, n, &i) != 0) return NULL;

    m = mount_find_by_path(path);
    if (!m) {
        /* Auto-mount if the file exists. */
        FILE *test = fopen(path, "rb");
        if (!test) {
            fprintf(stderr, "?DV file '%s' not found (use MOUNT first)\n", path);
            return NULL;
        }
        fclose(test);
        m = mount_open(path);
        if (!m) return NULL;
        printf("(auto-mounted %s)\n", path);
    }
    *idx = i;
    return m;
}

/* ----------------------------------------------------------------------
 *   Individual commands
 * ---------------------------------------------------------------------- */

static int do_create(char *tokens[], int n) {
    char path[MOUNT_PATH_MAX];
    int i = 1;
    char letter = 0;
    Mount *m;

    if (i >= n) {
        fprintf(stderr, "?Usage: CREATE fn[.dsk] [/DL] [/RT11] [letter:]\n");
        return -1;
    }
    ensure_default_ext(tokens[i], path, sizeof(path));
    i++;

    if (consume_modifiers(tokens, n, &i) != 0) return -1;

    if (i < n && looks_like_letter(tokens[i])) {
        letter = (char)toupper((unsigned char)tokens[i][0]);
        i++;
    }
    if (i != n) {
        fprintf(stderr, "?Extra arguments after CREATE\n");
        return -1;
    }

    printf("Creating %u-byte image '%s'...\n",
           (unsigned)RT11_DV_BYTES, path);
    if (rt11_create_image(path) != 0) return -1;

    if (letter) {
        m = mount_open(path);
        if (!m) return -1;
        if (mount_assign_letter(m, letter) != 0) return -1;
        printf("Mounted %s on %c:\n", path, letter);
    }
    return 0;
}

static int do_mount(char *tokens[], int n) {
    char path[MOUNT_PATH_MAX];
    int i = 1;
    char letter = 0;
    Mount *m;

    if (i >= n) {
        fprintf(stderr, "?Usage: MOUNT fn[.dsk] [/DL] [/RT11] [letter:]\n");
        return -1;
    }
    ensure_default_ext(tokens[i], path, sizeof(path));
    i++;
    if (consume_modifiers(tokens, n, &i) != 0) return -1;

    if (i < n && looks_like_letter(tokens[i])) {
        letter = (char)toupper((unsigned char)tokens[i][0]);
        i++;
    }
    if (i != n) {
        fprintf(stderr, "?Extra arguments after MOUNT\n");
        return -1;
    }

    m = mount_open(path);
    if (!m) return -1;

    if (letter) {
        if (mount_assign_letter(m, letter) != 0) {
            fprintf(stderr, "?Bad letter %c\n", letter);
            return -1;
        }
        printf("Mounted %s on %c:\n", m->path, letter);
    } else {
        printf("Mounted %s\n", m->path);
    }
    return 0;
}

static int do_assgn(char *tokens[], int n) {
    int i = 1;
    char letter;
    char path[MOUNT_PATH_MAX];
    Mount *m;

    if (i >= n || !looks_like_letter(tokens[i])) {
        fprintf(stderr, "?Usage: ASSGN letter: fn[.dsk] [/DL] [/RT11]\n");
        return -1;
    }
    letter = (char)toupper((unsigned char)tokens[i][0]);
    i++;

    if (i >= n) {
        fprintf(stderr, "?Missing filename\n");
        return -1;
    }
    ensure_default_ext(tokens[i], path, sizeof(path));
    i++;
    if (consume_modifiers(tokens, n, &i) != 0) return -1;
    if (i != n) {
        fprintf(stderr, "?Extra arguments after ASSGN\n");
        return -1;
    }

    m = mount_find_by_path(path);
    if (!m) {
        m = mount_open(path);
        if (!m) return -1;
    }
    if (mount_assign_letter(m, letter) != 0) {
        fprintf(stderr, "?Bad letter %c\n", letter);
        return -1;
    }
    printf("Assigned %c: -> %s\n", letter, m->path);
    return 0;
}

/* Save blocks [from_blk .. end) from a mounted DV file into a freshly
 * allocated buffer.  Returns the buffer on success (caller frees) and
 * writes its size to *out_len; returns NULL and prints an error on
 * failure. */
static unsigned char *save_area(Mount *m, long from_blk, size_t *out_len) {
    if ((unsigned long)from_blk >= (unsigned long)m->total_blocks) {
        *out_len = 0;
        return NULL;
    }
    size_t len = (size_t)((unsigned long)m->total_blocks - (unsigned long)from_blk) * 512u;
    unsigned char *buf = (unsigned char *)malloc(len);
    if (!buf) {
        fprintf(stderr, "?Out of memory saving %zu bytes\n", len);
        return NULL;
    }
    if (fseek(m->fp, from_blk * 512L, SEEK_SET) != 0 ||
        fread(buf, 1, len, m->fp) != len) {
        fprintf(stderr, "?Cannot save blocks >= %ld from %s\n",
                from_blk, m->path);
        free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
}

static int restore_area(Mount *m, long from_blk,
                        const unsigned char *buf, size_t len) {
    if (fseek(m->fp, from_blk * 512L, SEEK_SET) != 0 ||
        fwrite(buf, 1, len, m->fp) != len) {
        fprintf(stderr, "?Cannot restore blocks >= %ld on %s\n",
                from_blk, m->path);
        return -1;
    }
    fflush(m->fp);
    return 0;
}

static int do_format(char *tokens[], int n) {
    int i = 1;
    Mount *m;
    Rt11Dev dev;
    ModFlags mods = {0};
    char volid[13] = "RT11A       ";

    /* MT: routes to tape formatting. */
    if (i < n &&
        (tokens[i][0] == 'M' || tokens[i][0] == 'm') &&
        (tokens[i][1] == 'T' || tokens[i][1] == 't') &&
        tokens[i][2] == ':' && tokens[i][3] == '\0')
    {
        m = find_tape();
        if (!m) {
            fprintf(stderr, "?No tape is mounted\n");
            return -1;
        }
        i++;
        /* /RT11 is silently accepted on tape too, but has no effect. */
        if (consume_modifiers_ex(tokens, n, &i, MOD_RT11, &mods) != 0) return -1;
        if (i != n) {
            fprintf(stderr, "?Extra arguments after FORMAT MT:\n");
            return -1;
        }
        printf("Formatting tape %s ...\n", m->path);
        if (mt_format(m, "RT11A", "") != 0) return -1;
        printf("Tape formatted (empty volume, RT-11 FSM layout).\n");
        return 0;
    }

    m = resolve_dv(tokens, n, &i);
    if (!m) return -1;

    /* FORMAT-specific modifiers: /RT11 = preserve directory area. */
    if (consume_modifiers_ex(tokens, n, &i, MOD_RT11, &mods) != 0) return -1;
    if (i != n) {
        fprintf(stderr, "?Extra arguments after FORMAT\n");
        return -1;
    }

    if (m->kind == MOUNT_KIND_MT) {
        printf("Formatting tape %s ...\n", m->path);
        if (mt_format(m, "RT11A", "") != 0) return -1;
        printf("Tape formatted (empty volume, RT-11 FSM layout).\n");
        return 0;
    }

    dev.fp           = m->fp;
    dev.path         = m->path;
    dev.total_blocks = m->total_blocks;

    if (mods.seen & MOD_RT11) {
        /* Preserve everything from the directory start onwards. */
        size_t saved_len = 0;
        unsigned char *saved = save_area(m, RT11_DIR_START_BLOCK, &saved_len);
        if (!saved) return -1;
        printf("Formatting boot/home blocks of %s (preserving directory) ...\n",
               m->path);
        if (rt11_format(&dev, volid, "            ") != 0) {
            free(saved);
            return -1;
        }
        if (restore_area(m, RT11_DIR_START_BLOCK, saved, saved_len) != 0) {
            free(saved);
            return -1;
        }
        free(saved);
        printf("Boot + home block rewritten. Directory area preserved.\n");
        return 0;
    }

    printf("Formatting %s ...\n", m->path);
    if (rt11_format(&dev, volid, "            ") != 0) return -1;
    printf("Format complete. %u free blocks available.\n",
           (unsigned)(RT11_DV_BLOCKS - (RT11_DIR_START_BLOCK +
                                        RT11_MAX_SEGMENTS * RT11_SEG_BLOCKS)));
    return 0;
}

/* ----------------------------------------------------------------------
 *   DIR command: see cmd_dir.c
 * ---------------------------------------------------------------------- */

/* Return 1 if arg looks like a DV-letter reference of the form "A:" or
 * "A:name.ext".  A reference is rejected if the next char after the colon
 * is a slash/backslash (to avoid confusing a Windows path like "C:\foo"
 * with a DV ref).  Note: callers that also accept tape syntax should run
 * parse_tape_ref first, since "MT:..." would otherwise match here as a
 * bare "M:" drive reference with garbage after the colon. */
static int parse_dv_letter_ref(const char *arg, char *letter, const char **name) {
    if (!arg || !arg[0]) return 0;
    if (!isalpha((unsigned char)arg[0])) return 0;
    if (arg[1] != ':') return 0;
    if (arg[2] == '\\' || arg[2] == '/') return 0;  /* Win path, not DV */
    *letter = (char)toupper((unsigned char)arg[0]);
    *name   = &arg[2];   /* may point at the terminating NUL */
    return 1;
}

/* Return 1 if arg starts with "MT:" (case-insensitive).  Sets *name to
 * point at the char immediately after the colon (may be the terminating
 * NUL if the caller wrote just "MT:"). */
static int parse_tape_ref(const char *arg, const char **name) {
    if (!arg || !arg[0]) return 0;
    if ((arg[0] != 'M' && arg[0] != 'm')) return 0;
    if ((arg[1] != 'T' && arg[1] != 't')) return 0;
    if (arg[2] != ':') return 0;
    *name = &arg[3];
    return 1;
}

/* Derive the basename (filename component) of a host path, lower-cased
 * or original case as given - used when we expand a '*' on one side into
 * the explicit filename known on the other side. */
static const char *host_basename(const char *path) {
    const char *p = path;
    const char *s1 = strrchr(p, '/');
    const char *s2 = strrchr(p, '\\');
    const char *s  = s1 > s2 ? s1 : s2;
    return s ? s + 1 : p;
}

/* ----------------------------------------------------------------------
 *   Case-conversion modes  /  wildcard matching  /  radix-50 decoding
 * ---------------------------------------------------------------------- */

typedef enum { CASE_UC = 0, CASE_LC = 1, CASE_NC = 2 } CaseMode;

/* Apply case mode to string `s` in place. */
static void apply_case(char *s, CaseMode mode) {
    if (mode == CASE_NC) return;
    for (; *s; s++) {
        if (mode == CASE_UC) *s = (char)toupper((unsigned char)*s);
        else                 *s = (char)tolower((unsigned char)*s);
    }
}

/* Given modifier flags, return the effective CASE_* mode. Default CASE_UC. */
static CaseMode case_mode_from_flags(const ModFlags *f) {
    if (f->seen & MOD_LC) return CASE_LC;
    if (f->seen & MOD_NC) return CASE_NC;
    return CASE_UC;
}

/* Return 1 iff s contains a wildcard character (* or ?). */
static int has_wildcard(const char *s) {
    for (; *s; s++) {
        if (*s == '*' || *s == '?') return 1;
    }
    return 0;
}

/* Classic recursive shell-glob matcher.  '*' matches zero-or-more chars,
 * '?' matches a single char.  Comparison is case-insensitive.  Note that
 * '.' is matched literally, which matches RT-11 naming conventions
 * (NAME.EXT with a literal dot separator). */
static int glob_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            while (pat[1] == '*') pat++;             /* collapse ** */
            if (pat[1] == '\0') return 1;            /* trailing *  */
            pat++;
            while (*name) {
                if (glob_match(pat, name)) return 1;
                name++;
            }
            return glob_match(pat, name);
        } else if (*pat == '?') {
            if (!*name) return 0;
            pat++; name++;
        } else {
            if (toupper((unsigned char)*pat) != toupper((unsigned char)*name))
                return 0;
            pat++; name++;
        }
    }
    return *name == '\0';
}

/* Radix-50 character set used by PDP-11 / RT-11. 40 characters, packed
 * 3 per 16-bit word.  Index 0..39: space, A..Z, $, ., and the digits. */
static const char RAD50_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789";

/* Decode one 16-bit radix-50 word into 3 chars (no trailing NUL added). */
static void rad50_decode(uint16_t w, char out[3]) {
    int c0 = (w / (050 * 050)) % 050;
    int c1 = (w / 050) % 050;
    int c2 = w % 050;
    out[0] = RAD50_CHARS[c0];
    out[1] = RAD50_CHARS[c1];
    out[2] = RAD50_CHARS[c2];
}

/* Little-endian 16-bit word read from buf[off]. */
static uint16_t le16(const unsigned char *buf, int off) {
    return (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
}

/* Keep pre-existing helpers alive even when the rewritten do_copy
 * doesn't reference them directly, so -Wunused-function stays quiet.
 * Called (harmlessly) from cmd_execute_line. */
static void suppress_unused_helpers(void) {
    (void)host_basename;
    (void)parse_dv_letter_ref;
    (void)parse_tape_ref;
}

/* Read one 512-byte logical block from a mounted DV image. */
static int read_block(Mount *m, long blk, unsigned char buf[512]) {
    if (fseek(m->fp, blk * 512L, SEEK_SET) != 0) {
        fprintf(stderr, "?seek to block %ld failed\n", blk);
        return -1;
    }
    if (fread(buf, 1, 512, m->fp) != 512) {
        fprintf(stderr, "?read block %ld failed\n", blk);
        return -1;
    }
    return 0;
}

/* ----------------------------------------------------------------------
 *   RT-11 directory walker
 * ---------------------------------------------------------------------- */

#define RT11_E_TENT  0x0200
#define RT11_E_MPD   0x0400
#define RT11_E_EOS   0x0800
#define RT11_E_PERM  0x2000

typedef struct {
    char     name[16];
    uint16_t blocks;
    uint16_t start_blk;
    uint16_t status;
} Rt11DirEnt;

typedef void (*rt11_dir_cb)(const Rt11DirEnt *e, void *arg);

static int rt11_walk_dir(Mount *m, rt11_dir_cb cb, void *arg) {
    unsigned char seg[1024];
    int seg_idx = 1;
    int walks = 0;
    const int MAX_WALK = 64;

    while (seg_idx > 0 && walks < MAX_WALK) {
        long blk0 = (long)RT11_DIR_START_BLOCK +
                    (seg_idx - 1) * (long)RT11_SEG_BLOCKS;
        if (read_block(m, blk0,     seg)       != 0) return -1;
        if (read_block(m, blk0 + 1, seg + 512) != 0) return -1;

        uint16_t next_seg  = le16(seg, 2);
        uint16_t extra_bpe = le16(seg, 6);
        uint16_t seg_start = le16(seg, 8);

        int entry_size = 14 + (int)extra_bpe;
        if (entry_size < 14 || entry_size > 64) {
            fprintf(stderr, "?Bogus directory entry size %d in seg %d\n",
                    entry_size, seg_idx);
            return -1;
        }

        int off = 10;
        uint16_t cur_blk = seg_start;
        while (off + 14 <= 1024) {
            uint16_t status = le16(seg, off);
            if (status & RT11_E_EOS) break;

            uint16_t blocks = le16(seg, off + 8);

            if ((status & RT11_E_PERM) && !(status & RT11_E_MPD)) {
                Rt11DirEnt e;
                char n1[3], n2[3], ex[3];
                rad50_decode(le16(seg, off + 2), n1);
                rad50_decode(le16(seg, off + 4), n2);
                rad50_decode(le16(seg, off + 6), ex);

                char base[7];
                memcpy(base,     n1, 3);
                memcpy(base + 3, n2, 3);
                base[6] = '\0';
                int be = 6;
                while (be > 0 && base[be - 1] == ' ') be--;
                base[be] = '\0';

                char extstr[4] = { ex[0], ex[1], ex[2], '\0' };
                int ee = 3;
                while (ee > 0 && extstr[ee - 1] == ' ') ee--;
                extstr[ee] = '\0';

                if (ee > 0)
                    snprintf(e.name, sizeof(e.name), "%s.%s", base, extstr);
                else
                    snprintf(e.name, sizeof(e.name), "%s", base);
                e.blocks    = blocks;
                e.start_blk = cur_blk;
                e.status    = status;
                cb(&e, arg);
            }
            cur_blk = (uint16_t)(cur_blk + blocks);
            off += entry_size;
        }

        seg_idx = next_seg;
        walks++;
    }
    return 0;
}

/* Parse "[dev:]name" into a FileRef. */
typedef struct {
    int   is_dev;
    int   is_tape;
    char  letter;
    const char *name;
} FileRef;

/* Parse "[g,m]NAME.EXT" (the part after the device letter for RSTS
 * COPY).  On success returns 1 and fills g, p, name (6+1), type (3+1).
 * Accepts decimal g/m up to 255 each.  Filename may be 1..6 chars,
 * extension 0..3.  Returns 0 if the input doesn't look like a PPN
 * spec (so the caller can fall back to other syntaxes). */
static int parse_rsts_ppn_name(const char *arg, int *g, int *p,
                               char name_out[7], char type_out[4]) {
    int  gv = 0, pv = 0;
    const char *q;
    int  i;
    name_out[0] = '\0'; type_out[0] = '\0';
    if (!arg || arg[0] != '[') return 0;
    q = arg + 1;
    while (*q >= '0' && *q <= '9') { gv = gv*10 + (*q - '0'); q++; }
    if (*q != ',' || gv > 255) return 0;
    q++;
    while (*q >= '0' && *q <= '9') { pv = pv*10 + (*q - '0'); q++; }
    if (*q != ']' || pv > 255) return 0;
    q++;
    /* Now q points past ']' -- name part. */
    for (i = 0; i < 6 && *q && *q != '.'; i++, q++) name_out[i] = *q;
    name_out[i] = '\0';
    if (i == 0) return 0;
    if (*q == '.') {
        q++;
        for (i = 0; i < 3 && *q; i++, q++) type_out[i] = *q;
        type_out[i] = '\0';
    }
    *g = gv; *p = pv;
    return 1;
}

static void parse_file_ref(const char *arg, FileRef *out) {
    out->is_dev  = 0;
    out->is_tape = 0;
    out->letter  = 0;
    out->name    = arg;

    if (!arg || !arg[0]) return;

    if ((arg[0] == 'M' || arg[0] == 'm') &&
        (arg[1] == 'T' || arg[1] == 't') &&
        arg[2] == ':')
    {
        out->is_dev  = 1;
        out->is_tape = 1;
        out->name    = &arg[3];
        return;
    }
    if (isalpha((unsigned char)arg[0]) && arg[1] == ':' &&
        arg[2] != '\\' && arg[2] != '/')
    {
        out->is_dev = 1;
        out->letter = (char)toupper((unsigned char)arg[0]);
        out->name   = &arg[2];
        return;
    }
}

/* Per-file context used while walking an RT-11 directory during a
 * wildcard COPY. */
typedef struct {
    const char  *pattern;
    Rt11Dev     *dev;
    const char  *dst_prefix;
    CaseMode     case_mode;
    int          copied;
    int          skipped_exist;
    int          errors;
} WildcardCtx;

static int file_exists_path(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void wildcard_copy_cb(const Rt11DirEnt *e, void *arg) {
    WildcardCtx *ctx = (WildcardCtx *)arg;
    if (!glob_match(ctx->pattern, e->name)) return;

    char host_name[64];
    strlcopy(host_name, e->name, sizeof(host_name));
    apply_case(host_name, ctx->case_mode);

    char host_path[MOUNT_PATH_MAX];
    if (ctx->dst_prefix && *ctx->dst_prefix)
        snprintf(host_path, sizeof(host_path), "%s%s",
                 ctx->dst_prefix, host_name);
    else
        strlcopy(host_path, host_name, sizeof(host_path));

    if (file_exists_path(host_path)) {
        fprintf(stderr, "?Host file '%s' already exists, skipped\n", host_path);
        ctx->skipped_exist++;
        return;
    }

    if (rt11_copy_out(ctx->dev, e->name, host_path) != 0) {
        ctx->errors++;
        return;
    }
    printf("  %-14s -> %s  (%u blocks)\n",
           e->name, host_path, (unsigned)e->blocks);
    ctx->copied++;
}

/* Return 1 if `path` looks like a "bare directory" destination, i.e. it
 * ends with '/' or '\'. */
static int path_is_dir_hint(const char *path) {
    size_t n = strlen(path);
    return n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\');
}

/* Apply case mode to the basename portion of `path` (in place). */
static void apply_case_to_basename(char *path, CaseMode mode) {
    if (mode == CASE_NC) return;
    char *s1 = strrchr(path, '/');
    char *s2 = strrchr(path, '\\');
    char *s  = s1 > s2 ? s1 : s2;
    char *base = s ? s + 1 : path;
    apply_case(base, mode);
}

static int do_copy(char *tokens[], int n) {
    int i;
    const char *src_arg;
    const char *dst_arg;
    ModFlags mods = {0};
    CaseMode cm;
    FileRef src, dst;
    int src_wild;
    Mount *m;
    Rt11Dev dev;
    char        joined[MOUNT_PATH_MAX];

    if (n < 3) {
        fprintf(stderr, "?Usage: COPY <src> <dst> [/RT11] [/UC|/LC|/NC]\n");
        fprintf(stderr, "        each side = host path, A:name, or MT:name\n");
        fprintf(stderr, "        wildcards (* ?) are valid only on <src>\n");
        return -1;
    }

    src_arg = tokens[1];
    dst_arg = tokens[2];
    i = 3;
    if (consume_modifiers_ex(tokens, n, &i,
                             MOD_RT11 | MOD_UC | MOD_LC | MOD_NC | MOD_DR,
                             &mods) != 0)
        return -1;
    if (i != n) {
        fprintf(stderr, "?Extra arguments after COPY\n");
        return -1;
    }
    cm = case_mode_from_flags(&mods);

    parse_file_ref(src_arg, &src);
    parse_file_ref(dst_arg, &dst);

    /* Wildcards only allowed on source side. */
    if (has_wildcard(dst.name)) {
        fprintf(stderr, "?Wildcards are only valid on the source side\n");
        return -1;
    }
    src_wild = has_wildcard(src.name);

    if (!src.is_dev && !dst.is_dev) {
        fprintf(stderr, "?COPY needs exactly one device side (A:/B:/.../MT:); "
                        "neither '%s' nor '%s' is a device reference\n",
                src_arg, dst_arg);
        return -1;
    }
    if (src.is_dev && dst.is_dev) {
        fprintf(stderr, "?Device-to-device COPY is not supported\n");
        return -1;
    }

    /* Wildcards require src to be a disk device and dst to be a host path. */
    if (src_wild) {
        if (!src.is_dev || src.is_tape) {
            fprintf(stderr, "?Wildcard COPY requires a disk source (A: ... Z:)\n");
            return -1;
        }
        if (dst.is_dev) {
            fprintf(stderr, "?Wildcard COPY cannot target a device\n");
            return -1;
        }
    }

    /* ------------------------------------------------------------------
     *  Tape operations (MT:)
     * ------------------------------------------------------------------ */
    if (src.is_tape) {
        if (!src.name || !*src.name) {
            fprintf(stderr, "?Missing tape filename after MT:\n");
            return -1;
        }
        m = find_tape();
        if (!m) {
            fprintf(stderr, "?No tape is mounted (use MOUNT fn.tap first)\n");
            return -1;
        }
        if (path_is_dir_hint(dst_arg)) {
            char nm[32];
            strlcopy(nm, src.name, sizeof(nm));
            apply_case(nm, cm);
            snprintf(joined, sizeof(joined), "%s%s", dst_arg, nm);
            return mt_copy_out(m, src.name, joined);
        }
        strlcopy(joined, dst_arg, sizeof(joined));
        apply_case_to_basename(joined, cm);
        return mt_copy_out(m, src.name, joined);
    }
    if (dst.is_tape) {
        m = find_tape();
        if (!m) {
            fprintf(stderr, "?No tape is mounted (use MOUNT fn.tap first)\n");
            return -1;
        }
        return mt_copy_in(m, src_arg,
                          (dst.name && *dst.name) ? dst.name : NULL);
    }

    /* ------------------------------------------------------------------
     *  Disk wildcard operations (A:*.ext -> host prefix)
     * ------------------------------------------------------------------ */
    if (src_wild) {
        WildcardCtx ctx;
        m = mount_find_by_letter(src.letter);
        if (!m) {
            fprintf(stderr, "?Drive letter %c: not assigned\n", src.letter);
            return -1;
        }
        if (m->kind == MOUNT_KIND_MT) {
            fprintf(stderr, "?Wildcard COPY not supported for tape sources\n");
            return -1;
        }

        /* Apply cwd prefix when src.name doesn't carry an explicit
         * "[g,p]".  This lets `COPY R:*.SAV BIN/` work after
         * `CD R:[1,2]`. */
        {
            static char cwd_prefixed_w[MOUNT_PATH_MAX];
            if (m->kind == MOUNT_KIND_DV && src.name[0] != '[' &&
                (m->cwd_g != 0 || m->cwd_p != 0)) {
                snprintf(cwd_prefixed_w, sizeof(cwd_prefixed_w),
                         "[%d,%d]%s", m->cwd_g, m->cwd_p, src.name);
                src.name = cwd_prefixed_w;
            }
        }

        /* ODS-1 (RSX) / RSTS wildcard: src.name is "[g,m]NAME.EXT".
         * Decide by sniffing the volume; the file pointer is not
         * shared between the two test calls (each seeks absolute). */
        if (m->kind == MOUNT_KIND_DV && src.name[0] == '[') {
            uint8_t blk1[512];
            ods1_test_t t;
            int ods1_ok = 0, rsts_ok = 0;
            /* Sniff ODS-1. */
            if (RT_FSEEK64(m->fp, 512LL, SEEK_SET) == 0 &&
                fread(blk1, 1, 512, m->fp) == 512) {
                t = ods1_test(2, m->total_blocks, blk1);
                ods1_ok = (t.result == ODS1_TEST_PASS);
            }
            /* Sniff RSTS (always - both could happen on weirdly
             * constructed images, but normally only one passes). */
            rsts_ok = rsts_test(m->fp, m->total_blocks);

            if (ods1_ok) {
                int  g, p;
                char rname[7], rtype[4];
                int  case_mode_int = (cm == CASE_LC) ? 1 :
                                     (cm == CASE_NC) ? 2 : 0;
                int  copied = 0, skipped = 0, errors = 0;
                if (!parse_rsts_ppn_name(src.name, &g, &p, rname, rtype)) {
                    fprintf(stderr,
                            "?Bad ODS-1 source spec '%s'\n", src.name);
                    return -1;
                }
                if (!path_is_dir_hint(dst_arg)) {
                    fprintf(stderr,
                            "?Wildcard COPY destination must end in '/' "
                            "or '\\\\'\n");
                    return -1;
                }
                if (util_mkdir_p(dst_arg) != 0) {
                    fprintf(stderr,
                            "?Cannot create destination directory '%s'\n",
                            dst_arg);
                    return -1;
                }
                printf("COPY %c:[%d,%d]%s%s%s -> %s  (ODS-1)\n",
                       src.letter, g, p, rname,
                       rtype[0] ? "." : "", rtype, dst_arg);
                return ods1_copy_wild(m, g, p, rname,
                                      rtype[0] ? rtype : "",
                                      dst_arg, case_mode_int,
                                      &copied, &skipped, &errors);
            }

            /* not ODS-1 -- try RSTS next */
            if (rsts_ok) {
                int  g, p;
                char rname[7], rtype[4];
                int  case_mode_int = (cm == CASE_LC) ? 1 :
                                     (cm == CASE_NC) ? 2 : 0;
                int  copied = 0, skipped = 0, errors = 0;
                if (!parse_rsts_ppn_name(src.name, &g, &p, rname, rtype)) {
                    fprintf(stderr,
                            "?Bad RSTS source spec '%s'\n", src.name);
                    return -1;
                }
                if (!path_is_dir_hint(dst_arg)) {
                    fprintf(stderr,
                            "?Wildcard COPY destination must end in '/' "
                            "or '\\\\'\n");
                    return -1;
                }
                if (util_mkdir_p(dst_arg) != 0) {
                    fprintf(stderr,
                            "?Cannot create destination directory '%s'\n",
                            dst_arg);
                    return -1;
                }
                printf("COPY %c:[%d,%d]%s%s%s -> %s  (RSTS)\n",
                       src.letter, g, p, rname,
                       rtype[0] ? "." : "", rtype, dst_arg);
                return rsts_copy_wild(m, g, p, rname,
                                      rtype[0] ? rtype : "",
                                      dst_arg, case_mode_int,
                                      &copied, &skipped, &errors);
            }
            /* Volume looks neither ODS-1 nor RSTS -- fall through to
             * RT-11 with a heads-up so the user knows. */
            fprintf(stderr,
                    "?'%s' looks like a [g,m] spec but the volume on %c: "
                    "is not ODS-1 nor RSTS/E (proceeding as RT-11)\n",
                    src.name, src.letter);
        }

        dev.fp           = m->fp;
        dev.path         = m->path;
        dev.total_blocks = m->total_blocks;

        ctx.pattern       = src.name;
        ctx.dev           = &dev;
        ctx.dst_prefix    = dst_arg;
        ctx.case_mode     = cm;
        ctx.copied        = 0;
        ctx.skipped_exist = 0;
        ctx.errors        = 0;

        if (path_is_dir_hint(dst_arg) && util_mkdir_p(dst_arg) != 0) {
            fprintf(stderr,
                    "?Cannot create destination directory '%s'\n", dst_arg);
            return -1;
        }
        printf("COPY %c:%s -> %s%s\n",
               src.letter, src.name,
               dst_arg, path_is_dir_hint(dst_arg) ? "" : " (prefix)");
        if (rt11_walk_dir(m, wildcard_copy_cb, &ctx) != 0) return -1;
        printf("COPY summary: %d copied, %d skipped (exist), %d errors\n",
               ctx.copied, ctx.skipped_exist, ctx.errors);
        return ctx.errors == 0 ? 0 : -1;
    }

    /* ------------------------------------------------------------------
     *  Single-file random-access disk operations (A:..Z:)
     * ------------------------------------------------------------------ */
    if (src.is_dev) {
        /* DV -> host */
        const char *dst_path;
        if (!src.name || !*src.name) {
            fprintf(stderr, "?Missing filename after %c:\n", src.letter);
            return -1;
        }
        m = mount_find_by_letter(src.letter);
        if (!m) {
            fprintf(stderr, "?Drive letter %c: not assigned\n", src.letter);
            return -1;
        }
        if (m->kind == MOUNT_KIND_MT) {
            return mt_copy_out(m, src.name, dst_arg);
        }

        /* If the mount has a cwd set and src.name doesn't carry an
         * explicit "[g,p]" prefix, synthesise one from cwd so the
         * RSTS/ODS-1 single-file branches below can find it.  Stored
         * in a local buffer that lives until end of function. */
        {
            static char cwd_prefixed[MOUNT_PATH_MAX];
            if (m->kind == MOUNT_KIND_DV && src.name[0] != '[' &&
                (m->cwd_g != 0 || m->cwd_p != 0)) {
                snprintf(cwd_prefixed, sizeof(cwd_prefixed),
                         "[%d,%d]%s", m->cwd_g, m->cwd_p, src.name);
                src.name = cwd_prefixed;
            }
        }

        /* ODS-1 (RSX) COPY: source is "[g,m]NAME.EXT" (no wildcard
         * since we're in the single-file branch). */
        if (m->kind == MOUNT_KIND_DV && src.name[0] == '[') {
            uint8_t blk1[512];
            ods1_test_t t;
            int ods1_ok = 0;
            if (RT_FSEEK64(m->fp, 512LL, SEEK_SET) == 0 &&
                fread(blk1, 1, 512, m->fp) == 512) {
                t = ods1_test(2, m->total_blocks, blk1);
                ods1_ok = (t.result == ODS1_TEST_PASS);
            }
            if (ods1_ok) {
                int  g, p;
                char rname[7], rtype[4];
                char dst_resolved[MOUNT_PATH_MAX];
                const char *dst_real;
                if (!parse_rsts_ppn_name(src.name, &g, &p, rname, rtype)) {
                    fprintf(stderr,
                            "?Bad ODS-1 source spec '%s' (expected "
                            "[g,m]NAME.EXT)\n", src.name);
                    return -1;
                }
                if (path_is_dir_hint(dst_arg)) {
                    char nm[16];
                    snprintf(nm, sizeof(nm), "%s%s%s",
                             rname, rtype[0] ? "." : "", rtype);
                    apply_case(nm, cm);
                    snprintf(dst_resolved, sizeof(dst_resolved),
                             "%s%s", dst_arg, nm);
                    dst_real = dst_resolved;
                } else {
                    strlcopy(dst_resolved, dst_arg, sizeof(dst_resolved));
                    apply_case_to_basename(dst_resolved, cm);
                    dst_real = dst_resolved;
                }
                return ods1_copy_out(m, g, p, rname,
                                     rtype[0] ? rtype : "", 0,
                                     dst_real);
            }
        }

        /* RSTS/E COPY: source must be "[g,p]NAME.EXT" (or NAME / EXT
         * may contain wildcards).  We only enter this branch when the
         * mounted DV sniffs as a RSTS pack. */
        if (m->kind == MOUNT_KIND_DV && src.name[0] == '[' &&
            rsts_test(m->fp, m->total_blocks)) {
            int  g, p;
            char rname[7], rtype[4];
            int  case_mode_int;
            if (!parse_rsts_ppn_name(src.name, &g, &p, rname, rtype)) {
                fprintf(stderr,
                        "?Bad RSTS source spec '%s' (expected "
                        "[g,p]NAME.EXT)\n", src.name);
                return -1;
            }
            case_mode_int = (cm == CASE_LC) ? 1 :
                            (cm == CASE_NC) ? 2 : 0;
            /* Wildcard? then go through rsts_copy_wild. */
            if (has_wildcard(rname) || has_wildcard(rtype)) {
                int copied = 0, skipped = 0, errors = 0;
                if (!path_is_dir_hint(dst_arg)) {
                    fprintf(stderr,
                            "?Wildcard COPY destination must end in '/' "
                            "or '\\\\'\n");
                    return -1;
                }
                printf("COPY R:[%d,%d]%s%s%s -> %s\n",
                       g, p, rname,
                       rtype[0] ? "." : "", rtype, dst_arg);
                return rsts_copy_wild(m, g, p, rname,
                                      rtype[0] ? rtype : "",
                                      dst_arg, case_mode_int,
                                      &copied, &skipped, &errors);
            }
            /* Single-file copy. */
            {
                char dst_resolved[MOUNT_PATH_MAX];
                const char *dst_real;
                if (path_is_dir_hint(dst_arg)) {
                    char nm[16];
                    snprintf(nm, sizeof(nm), "%s%s%s",
                             rname, rtype[0] ? "." : "", rtype);
                    apply_case(nm, cm);
                    snprintf(dst_resolved, sizeof(dst_resolved),
                             "%s%s", dst_arg, nm);
                    dst_real = dst_resolved;
                } else {
                    strlcopy(dst_resolved, dst_arg, sizeof(dst_resolved));
                    apply_case_to_basename(dst_resolved, cm);
                    dst_real = dst_resolved;
                }
                return rsts_copy_out(m, g, p, rname,
                                     rtype[0] ? rtype : NULL, dst_real);
            }
        }

        dev.fp           = m->fp;
        dev.path         = m->path;
        dev.total_blocks = m->total_blocks;

        if (path_is_dir_hint(dst_arg)) {
            char nm[32];
            strlcopy(nm, src.name, sizeof(nm));
            apply_case(nm, cm);
            snprintf(joined, sizeof(joined), "%s%s", dst_arg, nm);
            dst_path = joined;
        } else {
            strlcopy(joined, dst_arg, sizeof(joined));
            apply_case_to_basename(joined, cm);
            dst_path = joined;
        }
        return rt11_copy_out(&dev, src.name, dst_path);
    } else {
        /* host -> DV */
        m = mount_find_by_letter(dst.letter);
        if (!m) {
            fprintf(stderr, "?Drive letter %c: not assigned\n", dst.letter);
            return -1;
        }
        if (m->kind == MOUNT_KIND_MT) {
            return mt_copy_in(m, src_arg,
                              (dst.name && *dst.name) ? dst.name : NULL);
        }
        if (m->kind == MOUNT_KIND_TAR) {
            return tar_copy_in(m, src_arg,
                               (dst.name && *dst.name) ? dst.name : NULL);
        }

        /* Apply cwd prefix if dst.name doesn't carry an explicit "[g,m]"
         * and the mount has a cwd set.                                  */
        {
            static char dst_cwd_prefixed[MOUNT_PATH_MAX];
            if (m->kind == MOUNT_KIND_DV && dst.name && dst.name[0] != '[' &&
                (m->cwd_g != 0 || m->cwd_p != 0)) {
                snprintf(dst_cwd_prefixed, sizeof(dst_cwd_prefixed),
                         "[%d,%d]%s", m->cwd_g, m->cwd_p, dst.name);
                dst.name = dst_cwd_prefixed;
            }
        }

        /* ODS-1 (RSX) write path: dst is "[g,m]NAME.EXT". */
        if (m->kind == MOUNT_KIND_DV && dst.name && dst.name[0] == '[') {
            uint8_t blk1[512];
            ods1_test_t t;
            int ods1_ok = 0;
            if (RT_FSEEK64(m->fp, 512LL, SEEK_SET) == 0 &&
                fread(blk1, 1, 512, m->fp) == 512) {
                t = ods1_test(2, m->total_blocks, blk1);
                ods1_ok = (t.result == ODS1_TEST_PASS);
            }
            if (ods1_ok) {
                int  g, p;
                char rname[7], rtype[4];
                int  dry = (mods.seen & MOD_DR) ? 1 : 0;
                if (!parse_rsts_ppn_name(dst.name, &g, &p, rname, rtype)) {
                    fprintf(stderr,
                            "?Bad ODS-1 dest spec '%s' (expected "
                            "[g,m]NAME.EXT)\n", dst.name);
                    return -1;
                }
                return ods1_copy_in(m, g, p, rname,
                                    rtype[0] ? rtype : "",
                                    src_arg, dry);
            }
            /* RSTS write path. */
            if (rsts_test(m->fp, m->total_blocks)) {
                int  g, p;
                char rname[7], rtype[4];
                int  dry = (mods.seen & MOD_DR) ? 1 : 0;
                if (!parse_rsts_ppn_name(dst.name, &g, &p, rname, rtype)) {
                    fprintf(stderr,
                            "?Bad RSTS dest spec '%s' (expected "
                            "[g,p]NAME.EXT)\n", dst.name);
                    return -1;
                }
                return rsts_copy_in(m, g, p, rname,
                                    rtype[0] ? rtype : "",
                                    src_arg, dry);
            }
            fprintf(stderr,
                    "?Volume on %c: is neither ODS-1 nor RSTS/E\n",
                    dst.letter);
            return -1;
        }

        dev.fp           = m->fp;
        dev.path         = m->path;
        dev.total_blocks = m->total_blocks;
        return rt11_copy_in(&dev, src_arg,
                            (dst.name && *dst.name) ? dst.name : NULL);
    }
}

static int do_boot(char *tokens[], int n) {
    int i = 1;
    Mount *m;
    Rt11Dev dev;
    const char *monitor;
    const char *handler;

    m = resolve_dv(tokens, n, &i);
    if (!m) return -1;

    if (i >= n) {
        fprintf(stderr, "?Usage: BOOT <dv> <monitor-file> <handler-file>\n");
        return -1;
    }
    monitor = tokens[i++];
    if (i >= n) {
        fprintf(stderr, "?Usage: BOOT <dv> <monitor-file> <handler-file>\n");
        fprintf(stderr, "?Missing handler file (the device driver)\n");
        return -1;
    }
    handler = tokens[i++];
    if (i != n) {
        fprintf(stderr, "?Extra arguments after BOOT\n");
        return -1;
    }
    dev.fp           = m->fp;
    dev.path         = m->path;
    dev.total_blocks = m->total_blocks;
    return rt11_boot(&dev, monitor, handler);
}

static int do_ver(char *tokens[], int n) {
    (void)tokens;
    if (n != 1) {
        fprintf(stderr, "?VER takes no arguments\n");
        return -1;
    }
    printf("rt11dv version %s %s %s  (built %s %s)\n",
           RT11DV_VERSION, rt11dv_platform(), rt11dv_buildtype(),
           __DATE__, __TIME__);
    return 0;
}

/* ----------------------------------------------------------------------
 *   CD / PWD - per-mount working directory state
 *
 * Each Mount carries cwd_g/cwd_p (default 0/0 = no cwd set / root).
 * `g_focus_letter` remembers the last drive letter the user CD'd into,
 * so a bare `CD ..` knows which mount to pop.
 * ---------------------------------------------------------------------- */

static char g_focus_letter = 0;

/* Parse a "[g,p]" trailing the given pointer (q points just past `[`).
 * Returns 1 + sets *gout/*pout on success, 0 otherwise. */
static int cd_parse_brackets(const char *q, int *gout, int *pout) {
    int gv = 0, pv = 0;
    while (*q >= '0' && *q <= '9') { gv = gv*10 + (*q - '0'); q++; }
    if (*q != ',' || gv > 255) return 0;
    q++;
    while (*q >= '0' && *q <= '9') { pv = pv*10 + (*q - '0'); q++; }
    if (*q != ']' || pv > 255) return 0;
    q++;
    if (*q != '\0') return 0;
    *gout = gv; *pout = pv;
    return 1;
}

static int do_cd(char *tokens[], int n) {
    const char *arg;
    Mount      *m;
    char        letter;

    if (n == 1) {
        fprintf(stderr,
                "?Usage: CD <letter>:[g,p]   - set working dir of <letter>:\n"
                "        CD <letter>:        - clear cwd of <letter>:\n"
                "        CD ..               - clear cwd of last-CD'd drive\n"
                "        CD [g,p]            - set cwd of last-CD'd drive\n");
        return -1;
    }
    if (n > 2) {
        fprintf(stderr, "?CD takes one argument\n");
        return -1;
    }
    arg = tokens[1];

    /* Form: "..  " -> clear focus mount's cwd. */
    if (strcmp(arg, "..") == 0) {
        if (!g_focus_letter) {
            fprintf(stderr, "?No CD focus drive (do `CD A:[g,p]` first)\n");
            return -1;
        }
        m = mount_find_by_letter(g_focus_letter);
        if (!m) {
            fprintf(stderr, "?Drive %c: not assigned anymore\n",
                    g_focus_letter);
            return -1;
        }
        m->cwd_g = 0; m->cwd_p = 0;
        printf("CD %c:..  (cwd cleared)\n", g_focus_letter);
        return 0;
    }

    /* Form: "[g,p]" -> set focus mount's cwd. */
    if (arg[0] == '[') {
        int g, p;
        if (!cd_parse_brackets(arg + 1, &g, &p)) {
            fprintf(stderr, "?Bad CD argument '%s' (expected [g,p])\n", arg);
            return -1;
        }
        if (!g_focus_letter) {
            fprintf(stderr,
                    "?No CD focus drive (qualify with letter, e.g. CD A:%s)\n",
                    arg);
            return -1;
        }
        m = mount_find_by_letter(g_focus_letter);
        if (!m) {
            fprintf(stderr, "?Drive %c: not assigned\n", g_focus_letter);
            return -1;
        }
        m->cwd_g = g; m->cwd_p = p;
        printf("CD %c:[%d,%d]\n", g_focus_letter, g, p);
        return 0;
    }

    /* Form: "<letter>:..." */
    if (isalpha((unsigned char)arg[0]) && arg[1] == ':') {
        letter = (char)toupper((unsigned char)arg[0]);
        m = mount_find_by_letter(letter);
        if (!m) {
            fprintf(stderr, "?Drive %c: not assigned\n", letter);
            return -1;
        }
        g_focus_letter = letter;
        if (arg[2] == '\0') {
            /* "A:" -> clear cwd. */
            m->cwd_g = 0; m->cwd_p = 0;
            printf("CD %c:  (cwd cleared, focus=%c:)\n", letter, letter);
            return 0;
        }
        if (strcmp(arg + 2, "..") == 0) {
            m->cwd_g = 0; m->cwd_p = 0;
            printf("CD %c:..  (cwd cleared)\n", letter);
            return 0;
        }
        if (arg[2] == '[') {
            int g, p;
            if (!cd_parse_brackets(arg + 3, &g, &p)) {
                fprintf(stderr,
                        "?Bad CD argument '%s' (expected %c:[g,p])\n",
                        arg, letter);
                return -1;
            }
            m->cwd_g = g; m->cwd_p = p;
            printf("CD %c:[%d,%d]\n", letter, g, p);
            return 0;
        }
        fprintf(stderr, "?Unrecognized CD form '%s'\n", arg);
        return -1;
    }

    fprintf(stderr, "?Unrecognized CD form '%s'\n", arg);
    return -1;
}

/* ----------------------------------------------------------------------
 *   EXEC <file> [<out>]   -- read commands from a file
 *   RET                   -- abort the running EXEC file
 *
 * Nested EXEC is allowed (g_exec_depth tracks recursion).  RET only
 * works when at least one EXEC is active; it sets g_exec_ret_pending
 * so the innermost EXEC loop bails on the next iteration.
 * ---------------------------------------------------------------------- */

static int g_exec_depth        = 0;
static int g_exec_ret_pending  = 0;

static int do_exec(char *tokens[], int n) {
    const char *fname;
    const char *outname = NULL;
    FILE       *fp;
    int         saved_stdout_fd = -1;
    int         rc = 0;
    char        line[2048];

    if (n < 2 || n > 3) {
        fprintf(stderr, "?Usage: EXEC <file> [<out>]\n");
        return -1;
    }
    fname = tokens[1];
    if (n == 3) outname = tokens[2];

    fp = fopen(fname, "r");
    if (!fp) {
        fprintf(stderr, "?Cannot open '%s' for reading\n", fname);
        return -1;
    }

    /* Optional stdout redirect via dup/dup2 so the subsequent
     * commands' printf goes to <out>. */
    if (outname) {
        FILE *redir;
#if defined(_WIN32)
        saved_stdout_fd = _dup(_fileno(stdout));
#else
        saved_stdout_fd = dup(fileno(stdout));
#endif
        if (saved_stdout_fd < 0) {
            fprintf(stderr, "?Cannot save stdout for EXEC redirect\n");
            fclose(fp);
            return -1;
        }
        redir = freopen(outname, "w", stdout);
        if (!redir) {
            fprintf(stderr, "?Cannot open '%s' for output\n", outname);
#if defined(_WIN32)
            _close(saved_stdout_fd);
#else
            close(saved_stdout_fd);
#endif
            fclose(fp);
            return -1;
        }
    }

    g_exec_depth++;
    while (fgets(line, sizeof(line), fp)) {
        int   sub_rc;
        char *p;
        /* Strip trailing CR/LF. */
        for (p = line; *p; p++) { /* nothing */ }
        while (p > line && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '))
            *(--p) = '\0';
        /* Skip blank lines + ';' or '!' comments. */
        for (p = line; *p && isspace((unsigned char)*p); p++) {}
        if (*p == '\0' || *p == '#' || *p == ';' || *p == '!') continue;
        /* Echo the command to stdout so it lands in the redirected
         * file and the log reads as command/result/command/result.
         * ECHO is special-cased: it already prints its own visible
         * line, so doubling the "> ECHO ..." would just clutter. */
        {
            int is_echo =
                (p[0]=='E'||p[0]=='e') && (p[1]=='C'||p[1]=='c') &&
                (p[2]=='H'||p[2]=='h') && (p[3]=='O'||p[3]=='o') &&
                (p[4]==' '||p[4]=='\t'||p[4]=='\0');
            if (!is_echo) {
                printf("> %s\n", p);
                fflush(stdout);
            }
        }
        sub_rc = cmd_execute_line(line);
        /* Flush after each command so its output lands in the log
         * before the next command's echo line.  Important when stdout
         * is fully buffered (the typical case after freopen to a
         * regular file). */
        fflush(stdout);
        if (sub_rc == -99) { rc = -99; break; }   /* EXIT inside EXEC */
        if (g_exec_ret_pending) {
            g_exec_ret_pending = 0;
            break;
        }
    }
    g_exec_depth--;
    fclose(fp);

    if (outname) {
        fflush(stdout);
#if defined(_WIN32)
        _dup2(saved_stdout_fd, _fileno(stdout));
        _close(saved_stdout_fd);
#else
        dup2(saved_stdout_fd, fileno(stdout));
        close(saved_stdout_fd);
#endif
        clearerr(stdout);
    }
    return rc;
}

/* DEL <hostpath> -- delete a host file (NOT a file inside a mounted
 * volume).  Useful in EXEC scripts to clean up between runs.  Reports
 * a warning if the file doesn't exist instead of failing the script. */
static int do_del(char *tokens[], int n) {
    if (n != 2) {
        fprintf(stderr, "?Usage: DEL <hostpath>\n");
        return -1;
    }
    if (remove(tokens[1]) != 0) {
        /* Not an error -- just inform.  Lets EXEC scripts unconditionally
         * clean up without failing on first run. */
        printf("(DEL: '%s' not found, nothing to remove)\n", tokens[1]);
        return 0;
    }
    printf("Deleted '%s'\n", tokens[1]);
    return 0;
}

/* ECHO -- prints its arguments (joined with single spaces) followed by
 * a newline.  Useful as a section marker inside EXEC files. */
static int do_echo(char *tokens[], int n) {
    int i;
    for (i = 1; i < n; i++) {
        if (i > 1) fputc(' ', stdout);
        fputs(tokens[i], stdout);
    }
    fputc('\n', stdout);
    return 0;
}

static int do_ret(char *tokens[], int n) {
    (void)tokens;
    if (n != 1) {
        fprintf(stderr, "?RET takes no arguments\n");
        return -1;
    }
    if (g_exec_depth == 0) {
        fprintf(stderr, "?RET only valid inside an EXEC file\n");
        return -1;
    }
    g_exec_ret_pending = 1;
    return 0;
}

static int do_pwd(char *tokens[], int n) {
    int i, any = 0;
    (void)tokens;
    if (n != 1) {
        fprintf(stderr, "?PWD takes no arguments\n");
        return -1;
    }
    /* Always print the heading so the line reads consistently in
     * EXEC log files (and is distinguishable from LIST's empty case). */
    printf("PWD - working directory state:\n");
    /* Walk drive letters A..Z (mount.c's table is private). */
    for (i = 0; i < 26; i++) {
        char    letter = (char)('A' + i);
        Mount  *m = mount_find_by_letter(letter);
        if (!m) continue;
        any = 1;
        if (m->cwd_g != 0 || m->cwd_p != 0) {
            printf("  %c: -> [%d,%d]   (%s)\n",
                   letter, m->cwd_g, m->cwd_p, m->path);
        } else {
            printf("  %c: -> (root/MFD)   (%s)\n", letter, m->path);
        }
    }
    if (!any) {
        printf("  (no mounted volumes)\n");
    }
    if (g_focus_letter) {
        printf("  Focus drive (for CD .. and CD [g,p]): %c:\n",
               g_focus_letter);
    }
    return 0;
}

static int do_umount(char *tokens[], int n) {
    int i = 1;
    Mount *m;
    if (i >= n) {
        fprintf(stderr, "?Usage: UMOUNT dv\n");
        return -1;
    }
    m = resolve_dv(tokens, n, &i);
    if (!m) return -1;
    if (i != n) {
        fprintf(stderr, "?Extra arguments after UMOUNT\n");
        return -1;
    }
    printf("Dismounted %s\n", m->path);
    mount_close(m);
    return 0;
}

/* ----------------------------------------------------------------------
 *   EXAM: block-level viewer for mounted DV images
 * ---------------------------------------------------------------------- */

/* Print one 16-byte row in classic "offset | 8 octal words | 16 ASCII" form. */
static void exam_print_row(long base, const unsigned char *buf, int len) {
    int i;
    printf("  %04lo:", base);
    for (i = 0; i < 8; i++) {
        int off = i * 2;
        if (off + 1 < len) {
            unsigned w = (unsigned)buf[off] | ((unsigned)buf[off + 1] << 8);
            printf(" %06o", w);
        } else if (off < len) {
            printf(" %03o   ", buf[off]);
        } else {
            printf("        ");
        }
    }
    printf("  |");
    for (i = 0; i < len; i++) {
        unsigned char c = buf[i];
        putchar((c >= 0x20 && c < 0x7f) ? (char)c : '.');
    }
    printf("|\n");
}

/* Dump a whole 512-byte block as 32 rows of 16 bytes. */
static void exam_dump_block(const unsigned char *buf) {
    int i;
    for (i = 0; i < 512; i += 16)
        exam_print_row((long)i, buf + i, 16);
}

/* Interpret block 1: the RT-11 home block.  Only the commonly-used fields
 * are reported here; the checksum is verified. */
static void exam_interpret_home(const unsigned char *buf) {
    unsigned i;
    uint16_t pack_cluster = le16(buf, 0722);
    uint16_t first_dir    = le16(buf, 0724);
    uint16_t version      = le16(buf, 0726);
    char     verstr[4];
    char     volid[13];
    char     owner[13];
    char     sysid[13];
    uint16_t chksum;
    uint16_t computed = 0;
    rt11_datefmt_t datefmt;

    rad50_decode(version, verstr); verstr[3] = 0;
    memcpy(volid, buf + 0730, 12); volid[12] = 0;
    memcpy(owner, buf + 0744, 12); owner[12] = 0;
    memcpy(sysid, buf + 0760, 12); sysid[12] = 0;
    chksum = le16(buf, 0776);
    for (i = 0; i < 0776; i += 2)
        computed = (uint16_t)(computed + le16(buf, (int)i));

    datefmt = rt11_datefmt_from_sysver(version);

    printf("\n  -- Interpreted as RT-11 home block --\n");
    printf("    [0722] Pack cluster size        : %u (0%o)\n",
           (unsigned)pack_cluster, (unsigned)pack_cluster);
    printf("    [0724] First directory segment  : %u (0%o)\n",
           (unsigned)first_dir, (unsigned)first_dir);
    printf("    [0726] System version word      : 0%o  (RAD50 \"%s\")\n",
           (unsigned)version, verstr);
    printf("           Directory date layout    : %s\n",
           rt11_datefmt_name(datefmt));
    printf("    [0730] Volume ID                : \"%s\"\n", volid);
    printf("    [0744] Owner name               : \"%s\"\n", owner);
    printf("    [0760] System ID                : \"%s\"\n", sysid);
    printf("    [0776] Checksum (stored)        : 0%o\n", (unsigned)chksum);
    printf("           Checksum (computed)      : 0%o  %s\n",
           (unsigned)computed,
           (computed == chksum) ? "[OK]" : "[MISMATCH]");
}

/* Interpret a 1024-byte directory segment pair.  `buf2` must point to the
 * full 1024 bytes (i.e. two consecutive blocks concatenated). */
static void exam_interpret_dirseg(const unsigned char *buf2) {
    uint16_t nsegs     = le16(buf2, 0);
    uint16_t next_seg  = le16(buf2, 2);
    uint16_t highest   = le16(buf2, 4);
    uint16_t extra_bpe = le16(buf2, 6);
    uint16_t seg_start = le16(buf2, 8);
    int      entry_size = 14 + (int)extra_bpe;
    int      off, shown;
    uint16_t cur_blk = seg_start;

    printf("\n  -- Interpreted as RT-11 directory segment header --\n");
    printf("    [0000] Total segments           : %u\n", (unsigned)nsegs);
    printf("    [0002] Next segment (0=last)    : %u\n", (unsigned)next_seg);
    printf("    [0004] Highest segment in use   : %u\n", (unsigned)highest);
    printf("    [0006] Extra bytes / entry      : %u  (entry size %d)\n",
           (unsigned)extra_bpe, entry_size);
    printf("    [0010] First data block of seg  : %u\n", (unsigned)seg_start);

    if (entry_size < 14 || entry_size > 64) {
        printf("    (entry size looks bogus; skipping entry decode)\n");
        return;
    }

    printf("    Entries (off=byte offset within segment, blk=data block):\n");
    shown = 0;
    for (off = 10; off + 14 <= 1024; off += entry_size) {
        uint16_t status = le16(buf2, off);
        uint16_t n1     = le16(buf2, off + 2);
        uint16_t n2     = le16(buf2, off + 4);
        uint16_t ex     = le16(buf2, off + 6);
        uint16_t blocks = le16(buf2, off + 8);
        uint16_t chan   = le16(buf2, off + 10);
        uint16_t job    = le16(buf2, off + 12);
        char     c1[3], c2[3], c3[3];
        const char *kind;
        if (status & 0x0800) { kind = "E_EOS (end of segment)"; }
        else if (status & 0x2000) kind = "E_PERM";
        else if (status & 0x0400) kind = "E_MPD";
        else if (status & 0x0200) kind = "E_TENT";
        else                      kind = "E_EMPTY";

        rad50_decode(n1, c1);
        rad50_decode(n2, c2);
        rad50_decode(ex, c3);

        printf("      [off=%04o blk=%04o] st=0%06o %-22s %c%c%c%c%c%c.%c%c%c  blks=%u chan=%u job=%u\n",
               (unsigned)off, (unsigned)cur_blk, (unsigned)status, kind,
               c1[0], c1[1], c1[2], c2[0], c2[1], c2[2],
               c3[0], c3[1], c3[2],
               (unsigned)blocks, (unsigned)chan, (unsigned)job);

        if (status & 0x0800) break;
        cur_blk = (uint16_t)(cur_blk + blocks);
        if (++shown >= 32) {
            printf("      ... more entries not shown\n");
            break;
        }
    }
}

static int do_exam(char *tokens[], int n) {
    int i = 1;
    Mount *m;
    long blk;
    char *endp;
    unsigned char buf[512];

    m = resolve_dv(tokens, n, &i);
    if (!m) return -1;
    if (m->kind == MOUNT_KIND_MT) {
        fprintf(stderr, "?EXAM is only supported on disk (DV) images\n");
        return -1;
    }
    if (i >= n) {
        fprintf(stderr, "?Usage: EXAM [<dv>] <block>\n");
        return -1;
    }

    /* The block number may be decimal or octal (0-prefixed). */
    blk = strtol(tokens[i], &endp, 0);
    if (*endp != '\0' || blk < 0) {
        fprintf(stderr, "?Invalid block number '%s'\n", tokens[i]);
        return -1;
    }
    i++;
    if (i != n) {
        fprintf(stderr, "?Extra arguments after EXAM\n");
        return -1;
    }
    if ((unsigned long)blk >= (unsigned long)m->total_blocks) {
        fprintf(stderr, "?Block %ld out of range (device has %lu blocks)\n",
                blk, (unsigned long)m->total_blocks);
        return -1;
    }

    if (read_block(m, blk, buf) != 0) return -1;

    printf("%s  block %ld (0%lo)\n", m->path, blk, blk);
    exam_dump_block(buf);

    if (blk == 0) {
        printf("\n  -- RT-11 primary boot block (block 0) --\n");
        printf("    First 2 bytes = %03o %03o  (bootable if != 000 000)\n",
               buf[0], buf[1]);
    } else if (blk == 1) {
        /* Block 1 is "home block" in both worlds.  Try ODS-1 first
         * (it has a strong signature: dual checksums + "DECFILE11A");
         * fall back to RT-11 if it doesn't match.                    */
        ods1_test_t t = ods1_test(2, (uint32_t)m->total_blocks, buf);
        if (t.result == ODS1_TEST_PASS) {
            ods1_home_t h;
            ods1_parse_home(buf, &h);
            ods1_print_home(stdout, &h);
        } else {
            if (t.reason[0] != '\0') {
                printf("\n  -- ODS-1 detection: not Files-11 (%s) --\n",
                       t.reason);
            }
            exam_interpret_home(buf);
        }
    } else if (blk >= 2 && blk <= 5) {
        printf("\n  -- RT-11 secondary boot area (block %ld) --\n", blk);
    } else if (blk >= RT11_DIR_START_BLOCK) {
        /* Only try to interpret as a directory segment if it lines up with
         * an expected segment boundary. */
        long delta = blk - RT11_DIR_START_BLOCK;
        if ((delta % RT11_SEG_BLOCKS) == 0 &&
            (delta / RT11_SEG_BLOCKS) < RT11_MAX_SEGMENTS &&
            ((unsigned long)(blk + 1) < (unsigned long)m->total_blocks)) {
            unsigned char buf2[1024];
            memcpy(buf2, buf, 512);
            if (read_block(m, blk + 1, buf2 + 512) == 0) {
                uint16_t nsegs = le16(buf2, 0);
                if (nsegs > 0 && nsegs <= RT11_MAX_SEGMENTS)
                    exam_interpret_dirseg(buf2);
            }
        }
    }
    return 0;
}

/* ----------------------------------------------------------------------
 *   Dispatcher
 * ---------------------------------------------------------------------- */

void cmd_print_help(void) {
    puts("");
    puts("Commands (case-insensitive):");
    puts("  CREATE | C   fn[.dsk] [/DL] [/RT11] [A:]  Create 10 MB zero image");
    puts("  MOUNT  | M   fn[.dsk] [/DL] [/RT11] [A:]  Open an existing image");
    puts("                                              Extension picks the kind:");
    puts("                                                .dsk/.rk/.rl = RT-11 disk");
    puts("                                                .tap/.mt     = SIMH magtape");
    puts("                                                .tar         = POSIX ustar");
    puts("  ASSGN  | A   A: fn[.dsk] [/DL] [/RT11]    Assign a drive letter");
    puts("  FORMAT | F   <dv>|MT: [/RT11]             Write empty RT-11 fs / tape");
    puts("                                              /RT11 = keep directory");
    puts("                                              (rewrite only boot + home)");
    puts("  DIR          [path|<dv>|MT:] [[g,m]]      No arg = host cwd; name = host");
    puts("                                              path/file; A: or MT: = device.");
    puts("                                              If A: is a mounted .tar archive,");
    puts("                                              prints the ustar table-of-contents.");
    puts("                                              On ODS-1 (RSX) volumes, A:[g,m]");
    puts("                                              drills into the matching UFD.");
    puts("                                              On RSTS/E packs, prints pack");
    puts("                                              header + first-cluster MFD PPNs.");
    puts("  COPY | CP    <src> <dst> [/RT11] [/UC|/LC|/NC]");
    puts("                                            Copy between host and DV/MT");
    puts("                                            <src>,<dst> ::= [<dev:>]<fn>");
    puts("                                            <fn>        ::= name[.ext]");
    puts("                                                          | *[.ext] | *.*");
    puts("                                            /UC (default), /LC, /NC control");
    puts("                                            case of host-side filenames.");
    puts("                                            Wildcards are valid on <src>");
    puts("                                            only.  Device-to-device COPY is");
    puts("                                            not supported.");
    puts("                                              Examples:");
    puts("                                                COPY prog.sav A:");
    puts("                                                COPY A:prog.sav prog.sav /LC");
    puts("                                                COPY A:*.sav ./out/ /LC");
    puts("                                                COPY A:*.* ./out/");
    puts("                                                COPY MT:prog.sav prog.sav");
    puts("                                              For TAR archives:");
    puts("                                                COPY host.txt T:");
    puts("                                                COPY host.txt T:newname.txt");
    puts("                                                  (appends as ustar entry)");
    puts("                                              For ODS-1 (RSX) write:");
    puts("                                                COPY host.txt A:[g,m]NAME.EXT");
    puts("                                                COPY host.txt A:[g,m]NAME.EXT /DR");
    puts("                                                  (/DR = dry-run, no writes)");
    puts("                                              For RSTS/E write:");
    puts("                                                COPY host.txt R:[g,p]NAME.EXT");
    puts("                                                COPY host.txt R:[g,p]NAME.EXT /DR");
    puts("                                                  (limit: <=7 file clusters)");
    puts("                                              For RSTS/E packs:");
    puts("                                                COPY R:[g,p]NAME.EXT host.dat");
    puts("                                                COPY R:[g,p]*.EXT host_dir/");
    puts("                                                  (read-only; PPN required)");
    puts("  EXAM   | E   [<dv>] <block>               Show block as octal+ASCII dump");
    puts("                                            Block may be decimal or 0-octal");
    puts("                                            Blocks 0/1/2-5/6+ get an extra");
    puts("                                            RT-11 interpretation pass.");
    puts("  BOOT   | B   <dv> <monitor> <handler>     Write bootstrap");
    puts("                                              (both files mandatory)");
    puts("  UMOUNT | U   <dv>                         Dismount a DV");
    puts("  LIST   | L                                Show active mounts");
    puts("  VER    | V                                Show version + plat/build + build date");
    puts("  CD           <par>                        Set working dir for a hierarchical");
    puts("                                              volume.  par = A:[g,p] | A: | A:..");
    puts("                                              | [g,p] | ..  Affects DIR/COPY when");
    puts("                                              no [g,p] is given on the command.");
    puts("  PWD                                       List cwd of all mounted volumes");
    puts("  EXEC         <file> [<out>]               Run commands from <file>; when <out>");
    puts("                                              given, redirect stdout there until");
    puts("                                              the EXEC ends.  Lines starting with");
    puts("                                              # ; ! are comments.  Nesting OK.");
    puts("  RET                                       Abort the current EXEC file early");
    puts("                                              (only valid inside an EXEC).");
    puts("  ECHO         <text...>                    Print text to stdout (handy as");
    puts("                                              section marker inside EXEC files).");
    puts("  DEL          <hostpath>                   Delete a HOST file (not a file");
    puts("                                              inside a mounted volume).  No-op");
    puts("                                              if the file doesn't exist.");
    puts("  HELP   | ?                                This help");
    puts("  EXIT   | QUIT | Q                         Leave the REPL");
    puts("");
    puts("  <dv>  ::=  A:                        (a previously assigned letter)");
    puts("         |  filename[.dsk] [/DL] [/RT11]");
    puts("");
    puts("  Append '>> filename' to any command to send stdout to that file.");
    puts("  Anything from '#' to end of line is treated as a comment.");
    puts("  F3     repeat the last command");
    puts("");
}

/* ---------------------------------------------------------------------
 *   Optional '>> filename' stdout redirection
 *
 *   If the (already-tokenised) command line ends in a ">>" token followed
 *   by a filename token, we:
 *     - open that file in append text mode
 *     - save stdout's underlying fd and swap in the file's fd
 *     - run the command
 *     - restore stdout to the terminal
 *
 *   Errors (fprintf(stderr, ...)) are NOT captured - they still go to the
 *   terminal, which matches the spec's "redirect output" intent.
 * --------------------------------------------------------------------- */

typedef struct {
    int    active;        /* 1 if we actually redirected */
    int    saved_fd;      /* original stdout fd (via dup) */
    FILE  *fp;            /* open handle for the target file */
} RedirState;

static int redir_begin(RedirState *r, const char *path) {
    memset(r, 0, sizeof(*r));
    r->fp = fopen(path, "a");
    if (!r->fp) {
        fprintf(stderr, "?Cannot open '%s' for append: %s\n",
                path, strerror(errno));
        return -1;
    }
    fflush(stdout);
    r->saved_fd = RT_DUP(RT_FILENO(stdout));
    if (r->saved_fd < 0) {
        fprintf(stderr, "?Cannot dup stdout\n");
        fclose(r->fp);
        return -1;
    }
    if (RT_DUP2(RT_FILENO(r->fp), RT_FILENO(stdout)) < 0) {
        fprintf(stderr, "?Cannot redirect stdout\n");
        RT_CLOSE(r->saved_fd);
        fclose(r->fp);
        return -1;
    }
    r->active = 1;
    return 0;
}

static void redir_end(RedirState *r) {
    if (!r->active) return;
    fflush(stdout);
    RT_DUP2(r->saved_fd, RT_FILENO(stdout));
    RT_CLOSE(r->saved_fd);
    fclose(r->fp);
    r->active = 0;
}

/* Look for a trailing ">> filename" in tokens[] (or the concatenated token
 * ">>filename").  If found, return the filename pointer and adjust *pn
 * to drop those tokens.  Returns NULL if no redirect. */
static const char *consume_redirect(char *tokens[], int *pn) {
    int n = *pn;
    if (n >= 2 && strcmp(tokens[n - 2], ">>") == 0) {
        const char *path = tokens[n - 1];
        *pn = n - 2;
        return path;
    }
    if (n >= 1 && strncmp(tokens[n - 1], ">>", 2) == 0
        && tokens[n - 1][2] != '\0') {
        const char *path = tokens[n - 1] + 2;
        *pn = n - 1;
        return path;
    }
    return NULL;
}

int cmd_execute_line(char *line) {
    char *tokens[MAX_TOKENS];
    int n;
    const char *verb;
    const char *redir_path;
    RedirState redir = {0};
    int rc;

    /* strip trailing newline */
    {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
    }
    /* strip inline '#' comments (everything from first '#' to EOL) */
    {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
    }
    /* Keep a live reference so GCC doesn't flag the fn as dead. */
    (void)suppress_unused_helpers;

    n = split_tokens(line, tokens, MAX_TOKENS);
    if (n == 0) return 0;

    /* Pull off a trailing '>> file' if present. */
    redir_path = consume_redirect(tokens, &n);
    if (n == 0) {
        fprintf(stderr, "?Nothing to execute before '>>'\n");
        return -1;
    }
    if (redir_path) {
        if (redir_begin(&redir, redir_path) != 0) return -1;
    }

    verb = tokens[0];
    if      (strcieq(verb, "CREATE") || strcieq(verb, "C"))  rc = do_create(tokens, n);
    else if (strcieq(verb, "MOUNT")  || strcieq(verb, "M"))  rc = do_mount (tokens, n);
    else if (strcieq(verb, "ASSGN")  || strcieq(verb, "A"))  rc = do_assgn (tokens, n);
    else if (strcieq(verb, "FORMAT") || strcieq(verb, "F"))  rc = do_format(tokens, n);
    else if (strcieq(verb, "DIR"))                           rc = do_dir   (tokens, n);
    else if (strcieq(verb, "COPY")   || strcieq(verb, "CP")) rc = do_copy  (tokens, n);
    else if (strcieq(verb, "EXAM")   || strcieq(verb, "E"))  rc = do_exam  (tokens, n);
    else if (strcieq(verb, "BOOT")   || strcieq(verb, "B"))  rc = do_boot  (tokens, n);
    else if (strcieq(verb, "UMOUNT") || strcieq(verb, "U"))  rc = do_umount(tokens, n);
    else if (strcieq(verb, "LIST")   || strcieq(verb, "L"))  { mount_list(); rc = 0; }
    else if (strcieq(verb, "VER")    || strcieq(verb, "V"))  rc = do_ver   (tokens, n);
    else if (strcieq(verb, "CD"))                            rc = do_cd    (tokens, n);
    else if (strcieq(verb, "PWD"))                           rc = do_pwd   (tokens, n);
    else if (strcieq(verb, "EXEC"))                          rc = do_exec  (tokens, n);
    else if (strcieq(verb, "RET"))                           rc = do_ret   (tokens, n);
    else if (strcieq(verb, "ECHO"))                          rc = do_echo  (tokens, n);
    else if (strcieq(verb, "DEL"))                           rc = do_del   (tokens, n);
    else if (strcieq(verb, "HELP")   || strcieq(verb, "?"))  { cmd_print_help(); rc = 0; }
    else if (strcieq(verb, "EXIT") || strcieq(verb, "QUIT") || strcieq(verb, "Q")) rc = -99;
    else {
        fprintf(stderr, "?Unknown command '%s' (type HELP for a list)\n", verb);
        rc = -1;
    }

    redir_end(&redir);
    return rc;
}

/* end of file */


