
Commands (case-insensitive):
  CREATE | C   fn[.dsk] [/DL] [/RT11] [A:]  Create 10 MB zero image
  MOUNT  | M   fn[.dsk] [/DL] [/RT11] [A:]  Open an existing image
  ASSGN  | A   A: fn[.dsk] [/DL] [/RT11]    Assign a drive letter
  FORMAT | F   <dv>|MT: [/RT11]             Write empty RT-11 fs / tape
                                              /RT11 = keep directory
                                              (rewrite only boot + home)
  DIR          [path|<dv>|MT:]              No arg = host cwd; name = host
                                              path/file; A: or MT: = device
  COPY | CP    <src> <dst> [/RT11] [/UC|/LC|/NC]
                                            Copy between host and DV/MT
                                            <src>,<dst> ::= [<dev:>]<fn>
                                            <fn>        ::= name[.ext]
                                                          | *[.ext] | *.*
                                            /UC (default), /LC, /NC control
                                            case of host-side filenames.
                                            Wildcards are valid on <src>
                                            only.  Device-to-device COPY is
                                            not supported.
                                              Examples:
                                                COPY prog.sav A:
                                                COPY A:prog.sav prog.sav /LC
                                                COPY A:*.sav ./out/ /LC
                                                COPY A:*.* ./out/
                                                COPY MT:prog.sav prog.sav
  EXAM   | E   [<dv>] <block>               Show block as octal+ASCII dump
                                            Block may be decimal or 0-octal
                                            Blocks 0/1/2-5/6+ get an extra
                                            RT-11 interpretation pass.
  BOOT   | B   <dv> <monitor> <handler>     Write bootstrap
                                              (both files mandatory)
  UMOUNT | U   <dv>                         Dismount a DV
  LIST   | L                                Show active mounts
  VER    | V                                Show version + build date
  HELP   | ?                                This help
  EXIT   | QUIT | Q                         Leave the REPL

  <dv>  ::=  A:                        (a previously assigned letter)
         |  filename[.dsk] [/DL] [/RT11]

  Append '>> filename' to any command to send stdout to that file.
  Anything from '#' to end of line is treated as a comment.
  F3     repeat the last command

