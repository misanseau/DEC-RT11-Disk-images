/*
 * commands.h - Command parsing and dispatch for rt11dv.
 */
#ifndef RT11DV_COMMANDS_H
#define RT11DV_COMMANDS_H

/* Parse and execute one command line. Returns 0 on success, non-zero on
 * error (the error has already been printed to stderr). A special return
 * value of -99 means "user requested quit". */
int cmd_execute_line(char *line);

/* Print the built-in help for the REPL. */
void cmd_print_help(void);

#endif /* RT11DV_COMMANDS_H */
