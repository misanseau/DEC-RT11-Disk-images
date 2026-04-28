/*
 * mt.h - RT-11 File-Structured Magtape (FSM) operations.
 *
 * The on-tape layout follows the DEC RT-11 V&FF manual section 1.2.1.1
 * ("RT-11 File-Structured Magtape Format"):
 *
 *   VOL1
 *   HDR1 (file 1)
 *    * tape-mark
 *   data blocks (512-byte fixed-length records)
 *    * tape-mark
 *   EOF1 (file 1, with block count)
 *    * tape-mark
 *   HDR1 (file 2)
 *    ...
 *    * * *            (triple tape-mark = logical EOT)
 *
 * Each label is 80 ASCII bytes; each data record is 512 bytes.
 *
 * The physical container on disk is the widely-used SIMH ".tap" format:
 * every record is wrapped by two 4-byte little-endian length words, and
 * a length word of value 0 represents a tape mark.  This is the format
 * accepted by simh, E11, PUTR, and most other PDP-11 tape tools.
 */
#ifndef RT11DV_MT_H
#define RT11DV_MT_H

#include <stdint.h>

#include "mount.h"

#define MT_LABEL_BYTES   80u
#define MT_BLOCK_BYTES   512u
#define MT_RECORD_MAX    65535u  /* largest record we will read */

/* SIMH .tap length-word sentinels. */
#define MT_TAPE_MARK     0u
#define MT_EOM           0xFFFFFFFFu

/* Initialise an empty RT-11 FSM tape: write VOL1 + placeholder file
 * followed by the logical end-of-tape markers. */
int mt_format(Mount *m, const char *vol_id, const char *owner_name);

/* Print the directory listing of the tape (all files between VOL1 and
 * the logical EOT). */
int mt_cmd_dir(Mount *m);

/* Copy a file out of the tape into a host path.  The RT-11 filename is
 * matched case-insensitively against the HDR1 labels. */
int mt_copy_out(Mount *m, const char *tape_name, const char *host_path);

/* Append a host file to the tape as a new FSM file.  The trailing
 * logical-EOT markers are rewritten to make room.  If tape_name is
 * NULL or empty, the RT-11 filename is derived from the basename of
 * host_path. */
int mt_copy_in(Mount *m, const char *host_path, const char *tape_name);

#endif /* RT11DV_MT_H */
