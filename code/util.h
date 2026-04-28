/*
 * util.h - Helpers common to several modules.
 */
#ifndef RT11DV_UTIL_H
#define RT11DV_UTIL_H

/* Must come before <stdio.h> on glibc so fseeko/ftello are 64-bit.  Has no
 * effect on MSVC. */
#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
#endif

#include <stdint.h>
#include <stdio.h>
#if !defined(_MSC_VER)
#  include <sys/types.h>   /* off_t */
#endif

/* --- Portable 64-bit file positioning ---------------------------------
 * MSVC uses _fseeki64/_ftelli64, POSIX uses fseeko/ftello.  A DV is
 * 10 MB today so 32-bit offsets would suffice, but we keep it 64-bit
 * friendly so the forthcoming TAPE device (arbitrary length) works. */
#if defined(_MSC_VER)
#  define RT_FSEEK64(fp,off,whence)  _fseeki64((fp),(__int64)(off),(whence))
#  define RT_FTELL64(fp)             _ftelli64(fp)
#else
#  define RT_FSEEK64(fp,off,whence)  fseeko((fp),(off_t)(off),(whence))
#  define RT_FTELL64(fp)             ((long long)ftello(fp))
#endif

/* Case-insensitive compare. */
int strcieq(const char *a, const char *b);

/* Copy src to dst (up to dstsz-1 chars) and ASCIIZ-terminate.
 * Returns the length of src. */
size_t strlcopy(char *dst, const char *src, size_t dstsz);

/* Upper-case a string in place. */
void strupper(char *s);

/* Read a 16-bit little-endian word from buf at offset off. */
uint16_t rd_u16(const uint8_t *buf, size_t off);

/* Write a 16-bit little-endian word at buf[off]. */
void wr_u16(uint8_t *buf, size_t off, uint16_t v);

/* RT-11 directory-date layout evolved with the monitor version:
 *
 *   RT11_DATEFMT_V3  (RT-11 V3 / V4)
 *     bit 15     : unused (0)
 *     bits 14-10 : month (5 bits)
 *     bits  9- 5 : day
 *     bits  4- 0 : year - 1972       (range 1972..2003)
 *
 *   RT11_DATEFMT_V5  (RT-11 V5.0 .. V5.4)
 *     bits 15-14 : unused (0)
 *     bits 13-10 : month (4 bits)
 *     bits  9- 5 : day
 *     bits  4- 0 : year - 1972       (range 1972..2003)
 *
 *   RT11_DATEFMT_V55 (RT-11 V5.5 and later)
 *     bits 15-14 : high 2 bits of year-1972   (range 1972..2099)
 *     bits 13-10 : month (4 bits)
 *     bits  9- 5 : day
 *     bits  4- 0 : low  5 bits of year-1972
 *
 * On the wire, V5 and V55 are indistinguishable when the year <= 2003
 * (bits 15-14 are 0 in both cases).  The only semantic difference is
 * during encoding of post-2003 dates, which V5 cannot represent. */
typedef enum {
    RT11_DATEFMT_V3   = 0,
    RT11_DATEFMT_V5   = 1,
    RT11_DATEFMT_V55  = 2
} rt11_datefmt_t;

/* Map a RAD50 home-block SYSVER word (offset 0726) to the date format
 * that should be assumed for that volume.
 *   RAD50 "V3A" -> V3      (RT-11 V3/V4)
 *   RAD50 "V05" -> V55     (V5.0+ -- safe superset of V5/V55 on decode)
 *   anything else -> V55   (default for unknown / zero words)
 *
 * Decoding as V55 is non-destructive for any V5.x volume because when
 * bits 15-14 are zero the decoded value is identical to V5.  The caller
 * should use V5 instead of V55 only when it needs to emit a strict
 * V5.0-compatible date word. */
rt11_datefmt_t rt11_datefmt_from_sysver(uint16_t sysver);

/* Human-readable name of a date format (e.g. "V5.5+"). */
const char *rt11_datefmt_name(rt11_datefmt_t fmt);

/* Encode a date (year, month=1..12, day=1..31) as an RT-11 directory
 * date word using the requested on-disk layout.  Out-of-range values
 * are silently clamped to what the layout can represent. */
uint16_t rt11_encode_date(int year, int month, int day, rt11_datefmt_t fmt);

/* Decode an RT-11 directory date word using the requested on-disk
 * layout.  Returns 0 and fills *y,*m,*d on success; returns non-zero
 * for a zero word (no date stored). */
int rt11_decode_date(uint16_t word, rt11_datefmt_t fmt,
                     int *y, int *m, int *d);

/* Return today's (local time) year/month/day. */
void today_ymd(int *year, int *month, int *day);

/* Create a directory if it doesn't already exist.  Accepts a trailing
 * '/' or '\\' (stripped before mkdir).  Returns 0 if the directory now
 * exists (created or already present), -1 on real failure. */
int util_mkdir_p(const char *path);

#endif /* RT11DV_UTIL_H */
