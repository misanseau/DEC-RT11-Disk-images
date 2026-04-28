/*
 * tar.h - POSIX ustar archive (.tar) reader.
 *
 * On-disk format (per IEEE Std 1003.1, "ustar interchange format"):
 *
 *   Each archive member is preceded by a 512-byte header.  Headers and
 *   data are aligned to 512-byte block boundaries; data shorter than a
 *   block is zero-padded.  The end of the archive is marked by two
 *   consecutive all-zero blocks.
 *
 *   Header fields (all ASCII, NUL-padded; numeric fields are octal
 *   strings, sometimes space-padded):
 *
 *      0  100  name        file name (NUL-terminated unless full)
 *    100    8  mode        octal mode bits
 *    108    8  uid         octal owner uid
 *    116    8  gid         octal owner gid
 *    124   12  size        octal file size in bytes
 *    136   12  mtime       octal Unix epoch seconds
 *    148    8  chksum      octal additive checksum, treat field as 8 spaces
 *    156    1  typeflag    '0'=file '1'=hardlink '2'=symlink '5'=dir ...
 *    157  100  linkname
 *    257    6  magic       "ustar\0"  (POSIX) or "ustar "  (GNU)
 *    263    2  version     "00" (POSIX) or " \0" (GNU)
 *    265   32  uname       ASCII owner name
 *    297   32  gname       ASCII group name
 *    329    8  devmajor    octal
 *    337    8  devminor    octal
 *    345  155  prefix      filename prefix (POSIX); concatenated as
 *                          "<prefix>/<name>" when non-empty.
 *
 * For now this module is read-only (DIR + extract); creation/append
 * can come later.
 */
#ifndef RT11DV_TAR_H
#define RT11DV_TAR_H

#include "mount.h"

#define TAR_BLOCK_BYTES   512u
#define TAR_NAME_MAX      256u   /* prefix + '/' + name + NUL */

/* Print a directory listing of a mounted .tar archive.  Returns 0 on
 * success, -1 on read error.  Bad/corrupt headers are reported but do
 * not abort the walk -- the function will skip ahead one block and
 * keep going until it sees the two-zero-block end-of-archive marker
 * or runs out of file. */
int tar_cmd_dir(Mount *m);

/* Append one host file to the archive.  Walks the existing entries to
 * find the first zero block (start of the end-of-archive marker), and
 * overwrites that location with: 1 USTAR header + ceil(size/512)
 * data blocks + 2 trailing zero blocks.  Truncates the file at that
 * point so any pre-existing junk past the end-of-archive is removed.
 *
 * tape_name: if NULL or empty, the basename of host_path is used.
 *            Otherwise it's the literal name stored in the header.
 *
 * Returns 0 on success, -1 on any error (with stderr message). */
int tar_copy_in(Mount *m, const char *host_path, const char *tape_name);

#endif /* RT11DV_TAR_H */
