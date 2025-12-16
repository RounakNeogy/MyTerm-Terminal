#define _POSIX_C_SOURCE 200809L
#include "autocomplete.h"
#include "shell_tab.h"
#include "line_edit.h"   /* optional; safe if tab->editor is present */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/types.h>
#include <signal.h>


/* to notify GUI to redraw when we update tab output */
extern volatile sig_atomic_t need_redraw;

/* helper: compute longest common prefix length of an array of strings */
static size_t longest_common_prefix_len(char **arr, int n) {
    if (n <= 0) return 0;
    if (n == 1) return strlen(arr[0]);
    size_t idx = 0;
    while (1) {
        char c = arr[0][idx];
        if (c == '\0') break;
        for (int i = 1; i < n; ++i) {
            if (arr[i][idx] != c) return idx;
        }
        ++idx;
    }
    return idx;
}

/* Helper: replace token in buffer (for editor or plain input) */
static void replace_token_in_tab(Tab *t, int token_start, int token_len, const char *replacement) {
    if (!t || !replacement) return;

    if (t->editor) {
        /* build new full buffer and reset editor */
        const char *old = le_get_buffer(t->editor);
        size_t oldlen = le_get_length(t->editor);
        size_t rep_len = strlen(replacement);
        size_t newlen = token_start + rep_len + (oldlen - (token_start + token_len));
        char *newbuf = malloc(newlen + 1);
        if (!newbuf) return;
        /* prefix */
        if (token_start > 0) memcpy(newbuf, old, token_start);
        /* replacement */
        memcpy(newbuf + token_start, replacement, rep_len);
        /* suffix */
        if (oldlen > (size_t)token_start + token_len)
            memcpy(newbuf + token_start + rep_len, old + token_start + token_len, oldlen - (token_start + token_len));
        newbuf[newlen] = '\0';
        le_reset(t->editor);
        le_feed_bytes(t->editor, newbuf, newlen);
        free(newbuf);
    } else {
        /* modify t->input in place */
        int oldlen = t->input_len;
        int rep_len = (int)strlen(replacement);
        int newlen = token_start + rep_len + (oldlen - (token_start + token_len));
        if (newlen >= INPUT_MAX - 1) return; /* protect */
        memmove(t->input + token_start + rep_len,
                t->input + token_start + token_len,
                oldlen - (token_start + token_len) + 1);
        memcpy(t->input + token_start, replacement, rep_len);
        t->input_len = newlen;
        t->input[t->input_len] = '\0';
        t->input_pos = token_start + rep_len;
    }
}

/* Helper: free stored pending match state in tab (caller must hold lock) */
static void clear_comp_state_locked(Tab *t) {
    if (!t) return;
    if (t->comp_matches) {
        for (int i = 0; i < t->comp_count; ++i) {
            free(t->comp_matches[i]);
        }
        free(t->comp_matches);
        t->comp_matches = NULL;
    }
    if (t->comp_dir) { free(t->comp_dir); t->comp_dir = NULL; }
    t->comp_count = 0;
    t->comp_pending = 0;
    t->comp_token_start = 0;
    t->comp_token_len = 0;
}

/* Public: clear pending */
void autocomplete_clear(int tab_idx) {
    Tab *t = tabs_get(tab_idx);
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    clear_comp_state_locked(t);
    pthread_mutex_unlock(&t->lock);
}

/* Main try function */
int autocomplete_try(int tab_idx) {
    Tab *t = tabs_get(tab_idx);
    if (!t) return 0;

    /* get current buffer & token start/len depending on editor vs raw input */
    const char *buf = NULL;
    size_t buflen __attribute__((unused)) = 0;
    size_t cursor = 0;
    if (t->editor) {
        buf = le_get_buffer(t->editor);
        buflen = le_get_length(t->editor);
        cursor = le_get_cursor(t->editor);
    } else {
        buf = t->input;
        buflen = (size_t)t->input_len;
        cursor = (size_t)t->input_pos;
    }
    if (!buf) buf = "";

    /* find start of last token (byte index) before cursor */
    ssize_t tok_start = (ssize_t)cursor - 1;
    if (tok_start < 0) tok_start = 0;
    while (tok_start > 0 && (unsigned char)buf[tok_start] > ' ') tok_start--;
    if ((unsigned char)buf[tok_start] <= ' ' && buf[tok_start] != '\0') tok_start++;
    if (tok_start < 0) tok_start = 0;
    size_t token_start = (size_t)tok_start;
    size_t token_len = 0;
    if (cursor > token_start) token_len = cursor - token_start;

    /* token text */
    char token[INPUT_MAX];
    if (token_len >= sizeof(token)) token_len = sizeof(token)-1;
    memcpy(token, buf + token_start, token_len);
    token[token_len] = '\0';

    if (token_len == 0) return 0; /* nothing to complete */

    /* Support path prefix: if token contains a slash, split directory and base.
       If no slash typed, dirprefix = "" and comp_dir will be NULL. */
    char dirprefix[INPUT_MAX];
    dirprefix[0] = '\0';
    const char *base = token;
    char *last_slash = strrchr(token, '/');
    if (last_slash) {
        size_t dlen = (size_t)(last_slash - token) + 1; /* include the slash */
        if (dlen >= sizeof(dirprefix)) dlen = sizeof(dirprefix)-1;
        memcpy(dirprefix, token, dlen);
        dirprefix[dlen] = '\0';
        base = token + dlen;
    } else {
        dirprefix[0] = '\0';
        base = token;
    }

    /* Open directory to search. If dirprefix is empty -> search current dir ".".
       If dirprefix ends with '/', opendir(dirprefix) is okay for relative paths.
    */
    const char *opendir_path = NULL;
    if (dirprefix[0] == '\0') opendir_path = ".";
    else {
        /* if dirprefix is "./" treat it as "." (but we still consider it a directory typed) */
        if (strcmp(dirprefix, "./") == 0) opendir_path = ".";
        else {
            /* dirprefix may be "path/to/" -> opendir on that path (without further manipulation) */
            opendir_path = dirprefix;
        }
    }

    DIR *d = opendir(opendir_path);
    if (!d) return 0;

    struct dirent *ent;
    char *matches[256];
    int mcount = 0;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strncmp(ent->d_name, base, strlen(base)) == 0) {
            matches[mcount++] = strdup(ent->d_name);
            if (mcount >= (int)(sizeof(matches)/sizeof(matches[0]))) break;
        }
    }
    closedir(d);

    if (mcount == 0) {
        return 0;
    } else if (mcount == 1) {
        /* single match: build insertion string = dirprefix + match (only prepend if user typed a dir) */
        char ins[INPUT_MAX];
        if (last_slash) {
            /* user typed a directory component; preserve it */
            snprintf(ins, sizeof(ins), "%s%s", dirprefix, matches[0]);
        } else {
            /* no directory typed — insert only the basename */
            snprintf(ins, sizeof(ins), "%s", matches[0]);
        }

        if (t->editor) {
            le_replace_last_word(t->editor, ins);
        } else {
            replace_token_in_tab(t, (int)token_start, (int)token_len, ins);
        }
        free(matches[0]);
        return 1;
    }

    /* multiple matches: compute longest common prefix length of basenames */
    size_t common = longest_common_prefix_len(matches, mcount);
    size_t baselen = strlen(base);
    if (common > baselen) {
        /* extend partially to the common prefix */
        char ext[INPUT_MAX];
        size_t ext_len = common;
        if (ext_len >= sizeof(ext)) ext_len = sizeof(ext)-1;
        memcpy(ext, matches[0], ext_len);
        ext[ext_len] = '\0';

        char ins[INPUT_MAX];
        if (last_slash) {
            snprintf(ins, sizeof(ins), "%s%s", dirprefix, ext);
        } else {
            snprintf(ins, sizeof(ins), "%s", ext);
        }

        if (t->editor) le_replace_last_word(t->editor, ins);
        else replace_token_in_tab(t, (int)token_start, (int)token_len, ins);
        /* note: we still show the list below after partial extension */
    }

    /* print numbered choices and save comp state so main can handle numeric selection */
    pthread_mutex_lock(&t->lock);
    /* free any previous state first to be safe */
    clear_comp_state_locked(t);

    /* keep up to 9 choices (so single-digit selection works) */
    int keep = mcount < 9 ? mcount : 9;
    t->comp_matches = malloc(sizeof(char*) * keep);
    if (!t->comp_matches) {
        /* cleanup temp matches */
        for (int i = 0; i < mcount; ++i) free(matches[i]);
        pthread_mutex_unlock(&t->lock);
        return 0;
    }
    for (int i = 0; i < keep; ++i) {
        t->comp_matches[i] = matches[i]; /* ownership transferred */
    }
    /* free any extra matches beyond kept ones */
    for (int i = keep; i < mcount; ++i) free(matches[i]);

    t->comp_count = keep;
    t->comp_pending = 1;
    t->comp_token_start = (int)token_start;
    t->comp_token_len = (int)token_len;
    /* store comp_dir only if a directory was typed; otherwise keep NULL */
    if (last_slash) t->comp_dir = strdup(dirprefix); else t->comp_dir = NULL;

    /* build output list (numbered) */
    char out[8192];
    size_t off = 0;
    off += snprintf(out + off, sizeof(out) - off, "\n");
    for (int i = 0; i < keep; ++i) {
        off += snprintf(out + off, sizeof(out) - off, "%2d. %s\n", i + 1, t->comp_matches[i]);
    }
    /* unlock before appending output (tabs_append_output will handle notification) */
    pthread_mutex_unlock(&t->lock);

    tabs_append_output(tab_idx, out, (ssize_t)off);
    need_redraw = 1;
    return 2;
}

/* User picks a choice (1-based) */
int autocomplete_select(int tab_idx, int choice) {
    Tab *t = tabs_get(tab_idx);
    if (!t) return -1;
    pthread_mutex_lock(&t->lock);
    if (!t->comp_pending || choice < 1 || choice > t->comp_count) {
        pthread_mutex_unlock(&t->lock);
        return -1;
    }
    const char *chosen = t->comp_matches[choice-1];
    /* build replacement = comp_dir + chosen (only if comp_dir is non-NULL) */
    char *replacement = NULL;
    if (t->comp_dir) {
        size_t rep_len = strlen(t->comp_dir) + strlen(chosen);
        replacement = malloc(rep_len + 1);
        if (!replacement) { pthread_mutex_unlock(&t->lock); return -1; }
        snprintf(replacement, rep_len + 1, "%s%s", t->comp_dir, chosen);
    } else {
        replacement = strdup(chosen);
        if (!replacement) { pthread_mutex_unlock(&t->lock); return -1; }
    }

    replace_token_in_tab(t, t->comp_token_start, t->comp_token_len, replacement);
    free(replacement);

    /* cleanup pending state */
    clear_comp_state_locked(t);
    need_redraw = 1;
    pthread_mutex_unlock(&t->lock);
    return 0;
}
