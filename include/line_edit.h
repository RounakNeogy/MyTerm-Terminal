#ifndef LINE_EDIT_H
#define LINE_EDIT_H

#include <stddef.h>

#define LE_MAX_INPUT 4096
#define LE_MAX_PROMPT 128

typedef struct LineEditor LineEditor;

/* create/destroy */
LineEditor *le_create(const char *prompt);
void le_destroy(LineEditor *le);

/* Prompt control */
void le_set_prompt(LineEditor *le, const char *prompt);

/* feed bytes (one-by-one or multiple). These will update internal buffer & cursor. */
void le_feed_byte(LineEditor *le, unsigned char b);
void le_feed_bytes(LineEditor *le, const char *buf, size_t n);

/* get buffer / lengths */
const char *le_get_buffer(LineEditor *le);
size_t le_get_length(LineEditor *le);
size_t le_get_cursor(LineEditor *le);

/* reset/clear current input */
void le_reset(LineEditor *le);

/* In terminal mode this prints the prompt + buffer and positions cursor.
   In GUI mode you can use le_get_buffer()/le_get_cursor() and draw yourself. */
void le_redraw_terminal(LineEditor *le);

void le_set_term_mode(LineEditor *le, int enabled); /* 1 = terminal mode (writes to stdout), 0 = GUI mode */

/* helper: replace last whitespace-delimited token in editor buffer with s (utf-8 bytes) */
void le_replace_last_word(LineEditor *le, const char *s);
void le_backspace(LineEditor *le);

#endif /* LINE_EDIT_H */
