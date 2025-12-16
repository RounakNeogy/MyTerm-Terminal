#ifndef AUTOCOMPLETE_H
#define AUTOCOMPLETE_H

/* Attempt completion for the given tab.
 * Returns:
 *   0 = no matches (nothing done)
 *   1 = inserted/partially inserted (added characters)
 *   2 = multiple matches printed to tab output and completion pending (user must choose a number)
 */
int autocomplete_try(int tab_idx);

/* If a numbered choice was printed (1..9), call this to pick it.
 * choice is 1-based index of printed list (single digit).
 * Returns 0 on success, -1 on error/invalid.
 */
int autocomplete_select(int tab_idx, int choice);

/* Clear any pending completion (safe to call). */
void autocomplete_clear(int tab_idx);

#endif /* AUTOCOMPLETE_H */
