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
 * main.c - Entry point for rt11dv.
 *
 *   rt11dv.exe                     -> interactive REPL (prompt ">").
 *   rt11dv.exe <command and args>  -> execute one command, then exit.
 *
 * The specification example walks through:
 *     >C  d1.dsk A:
 *     >F  A:
 *     >B  A:
 *     >COPY macro.sav A:
 *     >DIR A:
 *
 * A single process invocation holds its mount/drive-letter table in
 * memory; drive-letter assignments do not persist across runs.
 */

//#define _CRT_SECURE_NO_WARNINGS  //ya esta definida

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "mount.h"

#define LINE_BUFFER_SIZE 2048

static int run_repl(void) {
    char line[LINE_BUFFER_SIZE];
    int  rc;

    puts("rt11dv - RT-11 / TSX+ virtual disk manipulator");
    puts("Type HELP for a list of commands, EXIT to quit.");
    puts("");

    for (;;) {
        fputs("> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            fputs("\n", stdout);
            break;
        }
        rc = cmd_execute_line(line);
        if (rc == -99) break;
    }
    return 0;
}

/* Re-assemble argv[1..argc-1] into a single command line so we can feed
 * the exact same parser used by the REPL. */
static int run_oneshot(int argc, char **argv) {
    size_t total = 1;
    int i;
    char *line;
    int rc;

    for (i = 1; i < argc; i++) {
        total += strlen(argv[i]) + 1;
    }
    line = (char *)malloc(total);
    if (!line) {
        fprintf(stderr, "?Out of memory\n");
        return 1;
    }
    line[0] = '\0';
    for (i = 1; i < argc; i++) {
        if (i > 1) strcat(line, " ");
        strcat(line, argv[i]);
    }
    rc = cmd_execute_line(line);
    free(line);
    if (rc == -99) return 0;
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    int rc;

    mount_init();

    if (argc > 1) {
        rc = run_oneshot(argc, argv);
    } else {
        rc = run_repl();
    }

    mount_shutdown();
    return rc;
}
