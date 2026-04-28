/*
 * rad50.c - RADIX-50 encoding / decoding implementation.
 */
#include "rad50.h"

#include <ctype.h>
#include <string.h>

/* Character table: index -> character. Position 29 is "unused"; we decode
 * it as '?' but refuse to encode it (caller gets mapped to space). */
static const char rad50_charset[40] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.?0123456789";

/* Map an ASCII character to its RAD50 code (0..39). Unknown characters
 * and anything outside the set return 0 (space). */
static int rad50_code_of(char c) {
    c = (char)toupper((unsigned char)c);
    if (c == ' ') return 0;
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c == '$') return 27;
    if (c == '.') return 28;
    /* value 29 reserved, do not emit */
    if (c >= '0' && c <= '9') return 30 + (c - '0');
    return 0; /* unknown -> space */
}

uint16_t rad50_encode3(const char *s) {
    int c1 = 0, c2 = 0, c3 = 0;
    if (s && s[0]) {
        c1 = rad50_code_of(s[0]);
        if (s[1]) {
            c2 = rad50_code_of(s[1]);
            if (s[2]) {
                c3 = rad50_code_of(s[2]);
            }
        }
    }
    return (uint16_t)(c1 * 40 * 40 + c2 * 40 + c3);
}

int rad50_encode_filename(const char *name, uint16_t out[3]) {
    char base[7];
    char ext[4];
    const char *dot;
    size_t nbase, next;
    size_t i;

    if (!name || !*name) return -1;

    dot = strrchr(name, '.');
    if (dot) {
        nbase = (size_t)(dot - name);
        next  = strlen(dot + 1);
    } else {
        nbase = strlen(name);
        next  = 0;
    }
    if (nbase == 0 || nbase > 6 || next > 3) return -1;

    /* pad base to 6 chars with spaces */
    for (i = 0; i < 6; i++) {
        base[i] = (i < nbase) ? name[i] : ' ';
    }
    base[6] = '\0';

    /* pad ext to 3 chars with spaces */
    for (i = 0; i < 3; i++) {
        ext[i] = (dot && i < next) ? dot[1 + i] : ' ';
    }
    ext[3] = '\0';

    out[0] = rad50_encode3(&base[0]);
    out[1] = rad50_encode3(&base[3]);
    out[2] = rad50_encode3(&ext[0]);
    return 0;
}

void rad50_decode3(uint16_t word, char dst[3]) {
    uint16_t c1, c2, c3;
    c1 = (uint16_t)(word / (40 * 40));
    c2 = (uint16_t)((word / 40) % 40);
    c3 = (uint16_t)(word % 40);
    dst[0] = (c1 < 40) ? rad50_charset[c1] : '?';
    dst[1] = (c2 < 40) ? rad50_charset[c2] : '?';
    dst[2] = (c3 < 40) ? rad50_charset[c3] : '?';
}

void rad50_decode_filename(uint16_t filname_hi, uint16_t filname_lo,
                           uint16_t ext, char dst[11]) {
    char name[6];
    char ex[3];
    int nlen;
    int elen;
    int i;

    rad50_decode3(filname_hi, &name[0]);
    rad50_decode3(filname_lo, &name[3]);
    rad50_decode3(ext, ex);

    /* Trim trailing blanks of name */
    nlen = 6;
    while (nlen > 0 && name[nlen - 1] == ' ') nlen--;

    /* Check if extension is empty (all blanks) */
    elen = 3;
    while (elen > 0 && ex[elen - 1] == ' ') elen--;

    for (i = 0; i < nlen; i++) dst[i] = name[i];
    if (elen > 0) {
        dst[nlen++] = '.';
        for (i = 0; i < elen; i++) dst[nlen++] = ex[i];
    }
    dst[nlen] = '\0';
}
