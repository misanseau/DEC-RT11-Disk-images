# rt11dv  (WORK IN PROGRESS)

Console utility for **Win32 (Visual Studio 2022)** that manipulates
`.dsk` image files in the **DEC RT-11 / TSX+** filesystem format
("virtual disks", DV) and SIMH `.tap` container files in the
**RT-11 File-Structured Magtape (FSM)** format ("MT:"). Written in
plain C11, no third-party dependencies, no MFC, no ATL.

Version 0.2 adds FSM magtape support (MOUNT/FORMAT/DIR/COPY on `MT:`)
and re-bases the `DIR` command so that it inspects the host filesystem
when no device is given.

Version 0.4 adds:

- **`COPY R:[g,p]NAME.EXT host.path`** + **wildcard variant**
  `COPY R:[g,p]*.EXT host_dir/`: extract one file or many from a
  mounted RSTS/E pack to a host file or directory.  `host_dir` is
  auto-created (single level of mkdir) if it doesn't already exist.
- **`COPY A:[g,m]NAME.EXT host.path`** + wildcard for **ODS-1 (RSX)**:
  same syntax for Files-11 packs.  Walks the file's map area to copy
  each retrieval extent.
- **RSTS/E COPY IN**: `COPY host.txt R:[g,p]NAME.EXT [/DR]`.  Locates
  SATT.SYS in [0,1], finds a contiguous run of free PCN bits,
  allocates 3 blockettes in the target UFD (Name + Accounting +
  Retrieval), patches the previous Name Entry tail's ULNK, writes the
  data and updates SATT.SYS.  `/DR` runs a dry-run.  Limitations:
  file must fit in <=7 file clusters (single Retrieval Entry; FCS=PCS,
  so for PCS=16 the cap is 7*16*512 = 56 KB; for PCS=1, only 3.5 KB);
  UFD must already have 3 free 16-byte blockettes; no
  name-collision detection.  **Always test on a copy first.**
- **ODS-1 (RSX) COPY IN**: `COPY host.txt A:[g,m]NAME.EXT [/DR]`.
  Allocates a fresh FID via the index file bitmap, allocates N
  contiguous blocks via BITMAP.SYS, builds a File Header Block (with
  RAD-50 9.3 filename, default protection, current date/time, single
  retrieval pointer, additive checksum), writes the data and the
  dirent into the UFD.  `/DR` runs a dry-run (prints the plan: which
  bitmap bits would flip, which LBN gets the FH, etc.) without
  touching the disk image.  Limitations: single contiguous extent,
  UFD must already have a free 16-byte slot, no version-collision
  detection.  **Always test on a copy of the disk first.**
- **TAR COPY IN**: `COPY host.txt T:` (or `COPY host.txt T:newname`)
  appends a host file to a mounted `.tar` archive as a ustar entry.
  Walks the existing entries to find the first zero block, overwrites
  the end-of-archive marker with: 1 USTAR header + ceil(size/512)
  data blocks + 2 fresh trailing zero blocks.  File mode hardcoded to
  0644; mtime preserved from the host file's stat.  Names > 99 chars
  rejected (GNU long-name on write not yet implemented).
- **`CD` / `PWD`** for navigating hierarchical volumes (RSTS/E and
  ODS-1).  Each mount carries its own working directory:

  ```
  > CD A:[1,2]      ! set cwd of A: to UFD [1,2]
  > DIR A:          ! same as DIR A:[1,2]
  > COPY A:*.SAV ./ ! same as COPY A:[1,2]*.SAV ./
  > CD ..           ! pop cwd of last-CD'd drive (back to MFD)
  > PWD             ! show cwd of every mount + focus drive
  ```

  CD .. and a bare CD [g,p] target the "focus" drive, which is the
  last drive mentioned with a qualified `CD <letter>:...`.  Walks MFD -> account -> UFD -> file's
  Retrieval Entry chain (ULNK + 7 cluster DCNs per blockette) to
  enumerate every file cluster (FCS contiguous blocks per DCN) and
  writes the data out, capped by the file's USIZ (or the URTS=0
  large-file convention for >65535-block files).  Verified by
  extracting `[0,1]INIT.SYS` from `rsts.dsk` (419 blocks / 214528
  bytes) and confirming the known RSTS error strings appear.
- **`VER` output now includes platform + build type**: e.g. `version
  0.4 x64 rel  (built ...)`.  Compile-time detection of x32/x64 and
  release/debug, works under both MSVC (`_WIN64`/`_DEBUG`) and
  GCC/clang (`__x86_64__`/`__LP64__` and `DEBUG`/`NDEBUG`).

Version 0.3 added:

- Read-only support for **POSIX ustar (`.tar`) archives**: `MOUNT foo.tar
  T:` followed by `DIR T:` prints the archive's table of contents.
- ODS-1 (Files-11 / RSX) **home-block decode** in `EXAM block 1`, with
  byte-offset annotations on every interpreted field.
- **`DIR` for ODS-1 (RSX) volumes**: bare `DIR A:` on a Files-11 disk
  walks the MFD and lists every UFD as `[g,m]`; `DIR A:[g,m]` enters
  that UFD and lists the files inside.
- **`DIR` for RSTS/E packs (first cut)**: `DIR A:` on a RSTS/E disk
  decodes the pack label (RDS level, PCS/DCS, pack ID) and reports
  the PPN accounts visible in the first MFD cluster.  Full GFD/UFD
  walking is on the to-do list.
- Internal refactor: `DIR` lives in its own `cmd_dir.c` module, with
  `cmd_ods1_dir.c` housing the ODS-1 walker and `rsts.c` the RSTS
  pack-label decoder.

Version 0.2 adds FSM magtape support (MOUNT/FORMAT/DIR/COPY on `MT:`)
and re-bases the `DIR` command so that it inspects the host filesystem
when no device is given.

The program implements this commands:

| Verb | Short | Description |
|------|-------|-------------|
| `CREATE` | `C` | Allocate a new 10 MB zero-filled image file (other sizes are mountable if produced elsewhere) |
| `FORMAT` | `F` | Write an empty RT-11 home block and directory (DV) or empty VOL1/HDR1/EOF1 layout (MT:) |
| `BOOT`   | `B` | Patch the bootstrap using a monitor + handler file stored in the DV (both names mandatory) |
| `DIR`    |     | No arg: list the host current directory. With a host path: list that file or folder. With `A:` / `MT:`: list the RT-11 directory; on ODS-1 (RSX) volumes, optional `A:[g,m]` enters that UFD; on a mounted `.tar`, prints the ustar table of contents |
| `MOUNT`  | `M` | Open an existing DV or tape image; optional trailing `letter:` also assigns it |
| `ASSGN`  | `A` | Assign a drive letter to a mounted DV |
| `COPY`   |     | Copy a file between host and DV or host and MT: (either direction); `*` reuses the name from the other side |
| `UMOUNT` | `U` | Dismount |
| `LIST`   | `L` | Show current mount / assignment table (shows DV vs MT kind) |
| `VER`    | `V` | Print version + platform + build type (x32/x64 rel/deb) and compile-time build date/time |
| `HELP`   | `?` | Print help |
| `EXIT` / `QUIT` | | Leave the REPL |

Any command line may end in `>> filename` to append all `stdout` output
produced by that command to a host file (errors still go to stderr).

A DV argument is either a drive letter (`A:`) or a filename. When no
extension is given, `.dsk` is assumed. The spec modifiers `/DL` and
`/RT11` are accepted and validated (they have no structural effect
today).

A tape is mounted by `MOUNT foo.tap` (the `.tap`/`.mt` extension
switches the mount to FSM mode). The single active tape drive is then
addressable as `MT:` for `DIR`, `FORMAT` and `COPY`. Tape files are
sequential, so tape-to-tape `COPY` is not supported and a tape is
always treated as one pass of data records.

Mount and drive-letter assignments live **in memory** for the duration
of one process invocation, so the REPL is where the full workflow
shines:

```
> C  d1.dsk A:
> F  A:
> COPY rt11sj.sys A:
> COPY dl.sys     A:
> B  A: rt11sj.sys dl.sys        ! both monitor and handler are mandatory
> DIR A:
> DIR A: >> listing.txt           ! append the directory to a host file
```

## Build (Visual Studio 2022)

1. Open `rt11dv.sln` in Visual Studio 2022.
2. Pick the configuration you want (`Debug|x64` is fine for development, x32 is ok).
3. `Build -> Build Solution` (or press `Ctrl+Shift+B`).

The project:

- Targets `Application` (console, `/SUBSYSTEM:CONSOLE`).
- Uses the v143 toolset with the Windows 10 SDK.
- Compiles every `.c` file as C (not C++) with `/std:c11`.
- Warns at `/W4`, with SDL checks enabled.
- Ships `Debug|Win32`, `Debug|x64`, `Release|Win32` and `Release|x64`
  configurations.

## Build from the command line

From a *x64 Native Tools Command Prompt for VS 2022*:

```
cl /nologo /W4 /TC /std:c11 /D_CRT_SECURE_NO_WARNINGS ^
   main.c commands.c cmd_dir.c cmd_ods1_dir.c mount.c mt.c ods1.c ^
   rad50.c rsts.c rt11.c tar.c util.c /Fe:rt11dv.exe
```

## Usage examples

Interactive:

```
> rt11dv
rt11dv - RT-11 / TSX+ virtual disk manipulator
Type HELP for a list of commands, EXIT to quit.

> C d1.dsk A:
Creating 10485760-byte image 'd1.dsk'...
Mounted d1.dsk on A:
> F A:
Formatting d1.dsk ...
Format complete. 20412 free blocks available.
> COPY macro.sav A:
Copied 'macro.sav' -> MACRO.SAV (...)
> DIR A:
 Volume ID: RT11A
 Owner    :

 MACRO.SAV     xx  18-Apr-2026     < UNUSED >  ...

 1 Files, xx Blocks
 .... Free blocks
>
```

One-shot (each invocation runs a single command):

```
rt11dv DIR d1.dsk           rt11dv C new.dsk
rt11dv COPY hello.txt d1.dsk /DL /RT11
```

Drive-letter assignments done on the command line are lost as soon as
the process exits - that's intentional.

## Supported filesystem operations

- Volume size: any multiple of 512 bytes large enough to hold the home
  block plus at least one directory segment is accepted. The image size
  is measured at `MOUNT` time and the total_segs / first_file_blk fields
  are read from directory segment 1, so real RT-11 distribution disks
  of sizes other than 10 MB (e.g. 5 MB DEC ALGOL shipping disks) mount
  and list correctly. `LIST` shows the block count of every mounted DV.
- Directory format: 1-31 segments of 2 blocks each, up to 72 entries
  per segment (when no extra info bytes are used). Entries may be
  `< UNUSED >`, `PERMANENT`, or `PROTECTED PERMANENT`. TSX+ extra
  bytes in directory entries are transparently handled on read.
- Home block: block #1 with a valid directory pointer, RAD50
  system-version word, a 12-char Volume ID, 12-char Owner Name, and
  the required home-block checksum.
- Bootstrap: the `BOOT` command reads the monitor's blocks 1-4 into
  the secondary-boot area (blocks 2-5 of the DV), patches in the
  monitor filename and the primary driver's `B$READ` offset, copies
  the handler's primary driver into block 0 and patches the device
  name. This matches the RT-11 INITIALIZE/BOOT logic in the reference
  DUP (`rtboot`).

## File layout

```
rt11dv/
├── rt11dv.sln                 # VS 2022 solution
├── README.md                  # this file
└── rt11dv/                    # VS 2022 project
    ├── rt11dv.vcxproj
    ├── rt11dv.vcxproj.filters
    ├── rt11dv.rc              # Win32 resource script (icon + version info)
    ├── rt11dv.ico             # Application icon (16/32/48/64/128/256)
    ├── resource.h             # Resource IDs (IDI_APPICON)
    ├── main.c                 # entry point / REPL
    ├── commands.c/.h          # command parser + dispatcher
    ├── cmd_internal.h         # internal interface for cmd_*.c sub-modules
    ├── cmd_dir.c              # DIR command (host + DV + MT + TAR dispatch)
    ├── cmd_ods1_dir.c         # DIR for Files-11 ODS-1 (MFD + UFD walker)
    ├── rsts.c/.h              # RSTS/E pack-label + first-cut MFD scan
    ├── rt11.c/.h              # RT-11 filesystem core (random access, DV)
    ├── mt.c/.h                # RT-11 FSM magtape + SIMH .tap container
    ├── tar.c/.h               # POSIX ustar (.tar) read-only listing
    ├── ods1.c/.h              # Files-11 ODS-1 decoders (RSX home block)
    ├── rad50.c/.h             # RADIX-50 encode/decode
    ├── mount.c/.h             # in-memory mount table (DV / MT / TAR per slot)
    └── util.c/.h              # portable helpers
```

## COPY in both directions

```
COPY <src> <dst>
```

Exactly one of the two sides must be a device reference. A device
reference is either a random-access drive letter `A:name.ext` (where
`A` was previously assigned with `ASSGN` or auto-mounted via
`CREATE ... A:`) or the sequential tape `MT:name.ext`. The other side
is a regular Windows path.

```
COPY prog.sav A:                    ! host -> DV, RT-11 name derived from basename
COPY prog.sav A:prog.sav            ! host -> DV, explicit RT-11 name
COPY A:prog.sav prog.sav            ! DV  -> host
COPY A:macro.sav C:\work\macro.sav  ! DV  -> host (absolute host path)
COPY A:PROG.SAV *                   ! DV  -> host, host name = PROG.SAV
COPY * A:PROG.SAV                   ! host -> DV, host path = PROG.SAV
COPY prog.sav MT:                   ! host -> tape, FSM name derived
COPY prog.sav MT:prog.sav           ! host -> tape, explicit FSM name
COPY MT:prog.sav prog.sav           ! tape -> host
COPY MT:PROG.SAV *                  ! tape -> host, host name = PROG.SAV
```

The token `*` is shorthand for *"use the filename from the other side"*;
it saves you from typing the same name twice when the host path is just
the bare RT-11 filename in the current directory.

Device-to-device `COPY` is not supported (neither DV-to-DV,
MT-to-MT, nor cross-kind). Copy through a host file as an
intermediate step.

RT-11 files are stored in whole 512-byte blocks and the filesystem does
not keep a separate byte-length field, so the extracted host file is
always block-aligned (a padding tail of zero bytes may appear at the
end). This matches how RT-11 itself handles `.SAV` and data files.

## Tape (MT:) quick tour

```
> MOUNT foo.tap
Mounted foo.tap
> FORMAT MT:
Formatting tape foo.tap ...
Tape formatted (empty volume, RT-11 FSM layout).
> COPY hello.txt MT:
> COPY readme.md MT:README.DAT
> DIR MT:
 Volume ID: RT11A   Owner:
  HELLO .TXT  nn blocks   26-Apr-26
  README.DAT  nn blocks   26-Apr-26

 2 Files, nn Blocks
> COPY MT:README.DAT back.md
```

Every record on disk is wrapped in the SIMH `.tap` framing: two 4-byte
little-endian length words surrounding each record, a length of 0 for a
tape mark and 0xFFFFFFFF for end-of-medium. The logical end-of-tape
sentinel is a double tape mark. This matches `simh`, `E11`, `PUTR` and
the other common PDP-11 tape tools.

## TAR (`.tar`) quick tour

`rt11dv` can list the contents of a plain (uncompressed) POSIX ustar
archive.  This is convenient when an old DEC kit ships as a flat tarball
of `.SAV` / `.MAC` files, before you commit to copying them onto an
RT-11 volume.

```
> MOUNT kit.tar T:
Mounted kit.tar on T:
> DIR T:
 Archive: kit.tar  (POSIX ustar)

  Type  Name                                         Modified               Size
  ----  -------------------------------------------- ----------------  ----------
  DIR   ./                                           2026-04-25 10:55     <DIR>
        ./hello.txt                                  2026-04-25 10:55     12 B
        ./prog.sav                                   2026-04-25 10:55    4.0 KB
  DIR   ./sub/                                       2026-04-25 10:55     <DIR>
        ./sub/notes.md                               2026-04-25 10:55      6 B

  5 entries, 4114 bytes total
```

Caveats:

- **Read-only.**  Only `DIR` is implemented; `COPY` from inside a
  `.tar` is not yet wired up.  Extract first with the host `tar` tool.
- **No transparent decompression.**  `.tar.gz` / `.tar.xz` / `.tar.bz2`
  are detected by their magic bytes and rejected with a clear message
  ("looks like a gzip-compressed file, decompress first").
- **Long names** (>100 chars) are reconstructed from the GNU `'L'`
  extension when present.  POSIX prefix+name splitting is also
  honoured.  PAX `'x'`/`'g'` extended headers are skipped (the
  underlying `name` field is shown instead of any `path=` override).

## ODS-1 (RSX / Files-11) DIR quick tour

ODS-1 volumes are detected automatically at `DIR` time by sniffing
block 1 for the `DECFILE11A` signature.  Two modes are supported:

```
> MOUNT rsx-src.dsk A:
Mounted rsx-src.dsk on A:
> DIR A:
 Directory of rsx-src.dsk  (Files-11 ODS-1, volume "UNMSRC")
  Owner: 1,1   Vol level: 0401   Max files: 629

  UIC         Filename       FID
  ---------   -------------  -----------
  <reserved>  INDEXF   .SYS;1     FID=[1,1]
  <reserved>  BITMAP   .SYS;1     FID=[2,2]
  <reserved>  BADBLK   .SYS;1     FID=[3,3]
  [  0,  0]   000000   .DIR;1     FID=[4,4]
  [  1,  1]   001001   .DIR;1     FID=[6,1]
  ...
  26 UFDs, 4 reserved files

> DIR A:[1,1]
  Filename        FID
  --------------  -----------
  EXEMC    .MLB;1      FID=[32,1]
  SYSLIB   .OLB;1      FID=[243,1]
  ...
  14 files
```

Caveats / future work:

- File numbers > 16 are now resolved via a true INDEXF.SYS retrieval
  walk (cached at the start of every `DIR`) so fragmented INDEXFs
  work correctly and out-of-range FIDs are detected instead of
  reading garbage.  Falls back to the historical naive linear formula
  if INDEXF.SYS itself can't be parsed.
- Map-area extension headers are not yet followed: a UFD whose first
  header is full will list the first batch only.
- COPY and write paths are not yet implemented for ODS-1 volumes.

## RSTS/E pack DIR (first cut)

When `DIR A:` is run on a disk image that sniffs as an RSTS/E pack
(USED flag at offset 2 of DCN 1, sane RDS level + power-of-two PCS +
printable RAD-50 pack ID), `rt11dv` prints the pack header and the
PPN entries it can spot in the first MFD cluster:

```
> MOUNT system.dsk A:
Mounted system.dsk on A:
> DIR A:
 Pack: SYSTEM  (RSTS/E RDS 0.0)
  PCS=16  DCS=4  MFD LBN=4  Pack status=0x4800

  Account / blockette PPN scan (first MFD cluster only)
  Blkt   PPN     Link   Words 4..7 (RAD-50 / hex)
  ----   -----   ----   --------------------------
  [01]  [  1,  1]  0030   SYSTEM   (7abb 7dd5)
  [03]  [  0,  1]  0050   SYSTEM   (7abb 7dd5)
  [05]  [  1,  2]  0080   SYSTEM   (7abb 7dd5)
  ...
  Note: full GFD/UFD/file walking is not yet implemented.
```

What's done:

- DCS auto-computation from the pack size.
- Pack label decode (RDS level, PCS, pack status, pack ID).
- Both RDS 0.0 (block 1 = MFD start) and RDS 1.X packs sniff as RSTS.
- **Stage 2 + 3:** linked-chain walk through MFD entries with full
  per-account UFD descent.  Entries are decoded against the
  authoritative Mayfield "RSTS/E Monitor Internals" ch.1 layout:
  - MFD Name Entry: PPN, password (RAD-50 6 chars), USTAT bits,
    accounting link, **UAR = DCN of UFD's first cluster**.
  - Each non-empty UFD is opened (UFD_LBN = UAR * DCS) and its Name
    Entries are walked: filename.ext (RAD-50 9 chars), USTAT/UPROT,
    accounting entry → file size in blocks, creation date (RSTS
    internal date `(year-1970)*1000 + day_of_year`), creation time
    (RSTS minutes-until-midnight encoding), RTS name, file cluster size.

- **Stage 4:** account vs file distinction via `US.UFD` bit (0x40) of
  USTAT.  In RDS 0.0 the MFD doubles as the UFD for [1,1], so a single
  link chain mixes account-pointer entries (US.UFD=1) and file entries
  (US.UFD=0); Stage 4 dispatches accordingly.  After scanning the MFD,
  each account with `UAR != 0` and PPN != [1,1] is recursively walked
  as a UFD.  URTS=0 large-file convention (file size = (URTS2<<16) |
  USIZ) is honoured.  Cross-block links are detected and warned about
  (multi-block walks come in Stage 5).

  Sample output against rsts.dsk (SYSTEM, 67 MB):

  ```
  Pack: SYSTEM  (RSTS/E RDS 0.0)
    PCS=16  DCS=4  MFD LBN=4  Pack status=0x4800

  [ MFD walk -- accounts and intermixed [1,1] files ]
  ACCT  [  1,  1]  pass=SYSTEM   stat=UFD+NK+NX  UAR=1     -> UFD@LBN=4
  ACCT  [  0,  1]  pass=SYSTEM   stat=UFD+NK+NX  UAR=5     -> UFD@LBN=20
  ACCT  [  1,  2]  pass=SYSTEM   stat=UFD+NK+NX  UAR=229   -> UFD@LBN=916
    FILE  SYSLIB.OLB     182 blk  23-Sep-1979 17:37  RTS=RSX
    FILE  RSXMAC.SML     153 blk  23-Sep-1979 17:12  RTS=RSX
    FILE  CSPCOM.OLB     207 blk  19-Sep-1979 15:56  RTS=RSX
    FILE  ODT   .OBJ       9 blk   6-Jun-1998 16:05
  ACCT  [100,  1]  pass=SYSTEM   stat=UFD+NK+NX  UAR=2901  -> UFD@LBN=11604

  [ UFD [0,1] @LBN=20 ]
    BADB  .SYS       0 blk   6-Jun-1998 13:36  RTS=RSTS
    SATT  .SYS       3 blk   6-Jun-1998 13:36  RTS=RSTS
    INIT  .SYS     419 blk   6-Jun-1998 13:31  RTS=RSTS
    ERR   .ERR      16 blk   6-Jun-1998 13:31  RTS=RSTS
    RSTS  .SIL     307 blk   6-Jun-1998 14:09  RTS=RT11
    BASIC .RTS      73 blk   6-Jun-1998 14:09  RTS=RT11
    RT11  .RTS      20 blk   6-Jun-1998 13:31  RTS=RSTS
    SWAP  .SYS    1024 blk   6-Jun-1998 14:37  RTS=RSTS

  [ UFD [100,1] @LBN=11604 ]
    SQRT  .BAS       1 blk   6-Jun-1998 16:49  RTS=BASIC
  ```

- **Stage 5:** multi-block directory walks via the cluster map at
  offset 0x1F0 of every directory block (Mayfield 1.2.4 / 1.2.10).
  The link-word decoder follows the (block, cluster, entry, flags)
  packing per Mayfield 1.2.12, with target LBN computed as
  `cluster_map[link.cluster]_DCN * DCS + link.block`.  The
  accounting-blockette read also crosses blocks, so file size,
  date, time and RTS now populate for every entry, not just those
  whose accounting happens to live in the same block as the name
  entry.  Realistic file counts and totals:

  | Pack         | Accounts | Files | Blocks used |
  | ------------ | -------- | ----- | ----------- |
  | rsts.dsk     | 4        | 389   | 18 267      |
  | sysgng.dsk   | 3        | 169   |  7 923      |
  | sysl1g.dsk   | 4        | 184   |  8 409      |
  | patchg.dsk   | 3        | 718   |  2 216      |

- **COPY out**: `COPY R:[g,p]NAME.EXT host.path` extracts a file from a
  mounted RSTS/E pack.  Walks MFD to find the account, the UFD to find
  the file, then follows the Retrieval Entry chain (ULNK + 7 cluster
  DCNs per blockette) to write `min(USIZ, FCS×clusters) × 512` bytes
  to the host file.  Verified by extracting `[0,1]INIT.SYS` from
  `rsts.dsk` (419 blocks / 214528 bytes) and confirming all the known
  RSTS/E error strings ("INIT bug - SATT.SYS non-existent at time of
  WOMP", bootable-device list, etc.) appear in the output.

What's still pending:

- COPY *in* (host -> RSTS): requires SATT.SYS allocation + creating
  Name/Accounting/Retrieval blockettes in the UFD.

## Limitations / future work

- Only the `/DL` device and `/RT11` operating system modifiers are accepted.
- `COPY` does DV-to-DV transfers by hand: `COPY A:x.sys x.tmp` then
  `COPY x.tmp B:x.sys`. A native `A:x B:y` path is not supported yet.
- There is no file deletion command; overwriting a file with another
  `COPY` reclaims the old space automatically.
- There is no `LD:` (logical disk / subdirectory) support. All file
  operations happen at the root of the volume.
- Tapes are append-only: writing a new file re-uses the current
  logical end-of-tape sentinel. There is no tape-rewrite / tape-delete.
- Tape files are always copied out as whole 512-byte records; any
  non-multiple-of-512 source is zero-padded on write.
- The home-block `BUP` area and the bad-block replacement table are left zero.
- TAR archives are listing-only: no `COPY` in or out of a mounted
  `.tar`, no creation or append, no automatic decompression.

## References

The on-disk layout and command semantics follow the DEC RT-11 V&FF
manual. Useful cross-checks while writing this implementation:

- **rt11fsLib** - VxWorks-era RT-11 filesystem library; a concise C
  implementation of the same directory/home-block walk used here.
- **kgober/FSX** ("File System Exchange") - a general-purpose utility
  for poking around disk images from many vintage operating systems,
  including RT-11. Good reference for the directory-segment walk and
  the RADIX-50 conversions.
- **AA-PD6PA-TC RT-11 Volume and File Formats Manual (Aug 91)** -
  sections 1.2 *Sequential-Access Volumes* and 1.2.1.1 *RT-11
  File-Structured Magtape Format*. The on-tape VOL1/HDR1/EOF1 label
  content and the data-record layout implemented in `mt.c` follow the
  tables in this reference.
- **SIMH .tap container** - the de-facto standard disk container for
  emulated magtape images used by `simh`, `E11`, `PUTR`, etc.

  ---- end ----
