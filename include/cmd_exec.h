#ifndef CMD_EXEC_H
#define CMD_EXEC_H

/* Run a command line (supports pipes, redirs, quoted tokens) in tab `tab_idx`.
 * Implementation will capture stdout/stderr and append into the tab's output. */
int cmd_exec_run_in_tab(int tab_idx, const char *cmdline);

/* Send SIGINT to the foreground job (process group) running in tab_idx.
 * Returns 0 on success, -1 if no foreground job or on error. */
int cmd_exec_interrupt_tab(int tab_idx);

/* Send SIGTSTP to the foreground job (process group) running in tab_idx
 * (user pressed Ctrl+Z) - returns 0 on success, -1 otherwise. */
int cmd_exec_suspend_tab(int tab_idx);

#endif /* CMD_EXEC_H */
