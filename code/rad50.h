/*
 * rad50.h - RADIX-50 (RAD50) encoding / decoding for RT-11.
 *
 * RT-11 packs 3 characters into a 16-bit word using base 40 (50 octal).
 * Character set (values 0..39):
 *   0     : space
 *   1-26  : 'A'..'Z'
 *   27    : '$'
 *   28    : '.'
 *   29    : unused / reserved (decoded as '?')
 *   30-39 : '0'..'9'
 *
 * Encoding: word = c1*40*40 + c2*40 + c3   (40 decimal = 50 octal)
 */
#ifndef RT11DV_RAD50_H
#define RT11DV_RAD50_H

#include <stdint.h>

/* Encode up to 3 ASCII characters into a single RAD50 16-bit word.
 * Non-RAD50 characters are mapped to space; input is upper-cased.
 * If the input string is shorter than 3 characters, missing positions
 * are treated as space. Returns the packed 16-bit value. */
uint16_t rad50_encode3(const char *s);

/* Encode a full filename.ext into three 16-bit RAD50 words:
 *   out[0], out[1] = filename (6 chars)
 *   out[2]         = extension (3 chars)
 * Returns 0 on success, non-zero if the filename is malformed. */
int rad50_encode_filename(const char *name, uint16_t out[3]);

/* Decode a single RAD50 word into 3 ASCII characters written at dst[0..2].
 * dst is NOT null-terminated. */
void rad50_decode3(uint16_t word, char dst[3]);

/* Decode filename (2 words) and extension (1 word) into a printable
 * "NAME.EXT" string. Trailing blanks in name are trimmed; if extension is
 * all blanks, no dot is written. dst must be at least 11 bytes. */
void rad50_decode_filename(uint16_t filname_hi, uint16_t filname_lo,
                           uint16_t ext, char dst[11]);

#endif /* RT11DV_RAD50_H */
