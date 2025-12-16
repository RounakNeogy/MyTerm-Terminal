#ifndef MULTIWATCH_H
#define MULTIWATCH_H

/* Start multiwatch for a tab.
   cmds: array of NUL-terminated command strings (caller may free them afterwards).
   ncmds: number of commands.
   Returns 0 on success, -1 on failure. */
int multiwatch_start(int tab_idx, const char **cmds, int ncmds);

/* Interrupt / stop multiwatch running in the given tab (like Ctrl+C).
   Returns 0 on success, -1 if no multiwatch running for that tab. */
int multiwatch_interrupt(int tab_idx);

#endif /* MULTIWATCH_H */
