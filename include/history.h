#ifndef HISTORY_H
#define HISTORY_H

#include <stddef.h>

/* Initialize history subsystem.
 * If path == NULL, uses $HOME/.myterm_history.
 * Returns 0 on success.
 */
int history_init(const char *path);

/* Flush history to disk (call on exit). */
void history_save(void);

/* Add a command to history (skips empty/whitespace-only). */
void history_add(const char *cmd);

/* Show most recent `max` commands in the given tab (uses tabs_append_output). */
void history_show_recent(int tab_idx, int max);

/* Search history for an exact most recent match; returns a strdup() which caller must free,
 * or NULL if not found. */
char *history_find_exact(const char *term);

/* Search history, and output best matches (or "No match...") to tab_idx.
 * This follows the spec in your assignment:
 * - If exact most recent match: prints that.
 * - Otherwise: prints commands with longest common substring length (> 2).
 * - If none > 2: prints "No match for search term in history".
 */
void history_search_and_output(int tab_idx, const char *term);


#endif /* HISTORY_H */
