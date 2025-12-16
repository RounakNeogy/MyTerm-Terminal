#define _POSIX_C_SOURCE 200809L
#include "history.h"
#include "shell_tab.h" /* to use tabs_append_output */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>     // for open(), O_CREAT, O_RDONLY, etc.  // for close(), fsync(), rename()
#include <sys/stat.h> 

#ifndef HIST_MAX
#define HIST_MAX 10000
#endif

/* Circular buffer */
static char *hist_buf[HIST_MAX];
static int hist_count = 0;   /* number of entries currently stored (<= HIST_MAX) */
static int hist_start = 0;   /* index of oldest entry in buffer (valid only when hist_count>0) */

static char history_path[PATH_MAX] = {0};
static int hist_pos = -1;


// static void free_all_hist(void) {
//     for (int i = 0; i < HIST_MAX; ++i) {
//         if (hist_buf[i]) { free(hist_buf[i]); hist_buf[i] = NULL; }
//     }
//     hist_count = 0;
//     hist_start = 0;
// }

/* internal: push strdup'd line (assumes cmd is non-null) */
static void hist_push_str(const char *s) {
    if (!s) return;
    char *line = strdup(s);
    if (!line) return;
    /* trim trailing newline/carriage */
    size_t L = strlen(line);
    while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r')) { line[--L] = '\0'; }

    /* skip empty/whitespace-only */
    int only_space = 1;
    for (size_t i = 0; i < L; ++i) if (!isspace((unsigned char)line[i])) { only_space = 0; break; }
    if (only_space) { free(line); return; }

    if (hist_count < HIST_MAX) {
        int idx = (hist_start + hist_count) % HIST_MAX;
        hist_buf[idx] = line;
        hist_count++;
    } else {
        /* overwrite oldest */
        free(hist_buf[hist_start]);
        hist_buf[hist_start] = line;
        hist_start = (hist_start + 1) % HIST_MAX;
    }
}

/* Expand ~/ to $HOME */
// static const char *expand_home_path(const char *path, char *out, size_t outlen) {
//     if (!path) return NULL;
//     if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
//         const char *home = getenv("HOME");
//         if (!home) return NULL;
//         snprintf(out, outlen, "%s%s", home, path+1);
//         return out;
//     } else {
//         strncpy(out, path, outlen-1);
//         out[outlen-1] = '\0';
//         return out;
//     }
// }

int history_init(const char *path) {
    /* set file path */
    if (path && path[0] != '\0') {
        strncpy(history_path, path, sizeof(history_path)-1);
        history_path[sizeof(history_path)-1] = '\0';
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0') {
            /* fallback: try getpwuid? but keep simple and return error */
            return -1;
        }
        /* build default path */
        snprintf(history_path, sizeof(history_path)-1, "%s/.myterm_history", home);
        history_path[sizeof(history_path)-1] = '\0';
    }

    /* Try to create the file if it doesn't exist (so perms/dir issues surface early) */
    {
        int fd = open(history_path, O_CREAT | O_RDONLY, 0644);
        if (fd >= 0) close(fd);
        /* if open failed, we continue — saving will fail later and caller will get an error on write. */
    }

    /* try to open and read up to HIST_MAX lines */
    FILE *f = fopen(history_path, "r");
    if (!f) {
        /* missing history file is not fatal; start with empty history */
        return 0;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t r;
    /* read all lines, but only keep last HIST_MAX */
    while ((r = getline(&line, &cap, f)) != -1) {
        /* strip newline */
        while (r > 0 && (line[r-1] == '\n' || line[r-1] == '\r')) { line[--r] = '\0'; }
        hist_push_str(line);
    }
    free(line);
    fclose(f);
    return 0;
}

void history_save(void) {
    if (history_path[0] == '\0') return;

    /* write to a temp file then rename to avoid corruption */
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", history_path, (int)getpid());

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        /* can't open temporary - try direct write as a fallback */
        FILE *f = fopen(history_path, "w");
        if (!f) return;
        for (int i = 0; i < hist_count; ++i) {
            int idx = (hist_start + i) % HIST_MAX;
            if (hist_buf[idx]) fprintf(f, "%s\n", hist_buf[idx]);
        }
        fclose(f);
        return;
    }

    /* write from oldest to newest */
    for (int i = 0; i < hist_count; ++i) {
        int idx = (hist_start + i) % HIST_MAX;
        if (hist_buf[idx]) {
            size_t len = strlen(hist_buf[idx]);
            ssize_t w = write(fd, hist_buf[idx], len);
            if (w < 0) { /* ignore and continue */ }
            write(fd, "\n", 1);
        }
    }

    /* flush to disk (best-effort) */
    fsync(fd);
    close(fd);

    /* atomically rename into place */
    rename(tmp_path, history_path);
}

void history_add(const char *cmd) {
    if (!cmd || cmd[0] == '\0') return;
    hist_push_str(cmd);
    /* reset hist_pos so subsequent history navigation starts at most-recent entry */
    hist_pos = -1;
}

/* internal helper: get index of entry i where i=0 is oldest, i=hist_count-1 most recent */
static const char *hist_index(int i) {
    if (i < 0 || i >= hist_count) return NULL;
    int idx = (hist_start + i) % HIST_MAX;
    return hist_buf[idx];
}

void history_show_recent(int tab_idx, int max) {
    if (max <= 0) max = 1000;
    if (hist_count == 0) {
        const char *msg = "history: no entries\n";
        tabs_append_output(tab_idx, msg, (ssize_t)strlen(msg));
        return;
    }
    int printed = 0;
    /* print most recent first as per spec */
    for (int i = 0; i < hist_count && printed < max; ++i) {
        int idx = (hist_count - 1) - i; /* most recent index in 0..hist_count-1 */
        const char *s = hist_index(idx);
        if (!s) continue;
        char line[4096];
        int L = snprintf(line, sizeof(line), "%s\n", s);
        tabs_append_output(tab_idx, line, L);
        printed++;
    }
}

/* longest common substring length between a and b */
static int lcs_len(const char *a, const char *b) {
    if (!a || !b) return 0;
    int na = (int)strlen(a);
    int nb = (int)strlen(b);
    if (na == 0 || nb == 0) return 0;

    /* DP with two rows to reduce memory */
    int *prev = calloc(nb + 1, sizeof(int));
    int *cur  = calloc(nb + 1, sizeof(int));
    if (!prev || !cur) {
        free(prev); free(cur);
        return 0;
    }
    int best = 0;
    for (int i = 1; i <= na; ++i) {
        for (int j = 1; j <= nb; ++j) {
            if (a[i-1] == b[j-1]) {
                cur[j] = prev[j-1] + 1;
                if (cur[j] > best) best = cur[j];
            } else {
                cur[j] = 0;
            }
        }
        /* swap */
        int *tmp = prev; prev = cur; cur = tmp;
        /* clear cur for next row (only need nb+1 zeros) */
        memset(cur, 0, (nb+1)*sizeof(int));
    }
    free(prev);
    free(cur);
    return best;
}

char *history_find_exact(const char *term) {
    if (!term || hist_count == 0) return NULL;
    /* search from most recent going backwards */
    for (int i = hist_count - 1; i >= 0; --i) {
        const char *s = hist_index(i);
        if (!s) continue;
        if (strcmp(s, term) == 0) {
            return strdup(s);
        }
    }
    return NULL;
}

void history_search_and_output(int tab_idx, const char *term) {
    if (!term || term[0] == '\0') {
        const char *msg = "Empty search term\n";
        tabs_append_output(tab_idx, msg, (ssize_t)strlen(msg));
        return;
    }

    /* 1) exact most recent match? */
    char *exact = history_find_exact(term);
    if (exact) {
        char out[4096];
        int L = snprintf(out, sizeof(out), "Exact match:\n%s\n", exact);
        tabs_append_output(tab_idx, out, L);
        free(exact);
        return;
    }

    /* 2) find commands with largest longest-common-substring > 2 */
    int best_len = 0;
    /* We'll collect indexes of candidates */
    int *cands = calloc(hist_count > 0 ? hist_count : 1, sizeof(int));
    int cands_n = 0;
    for (int i = 0; i < hist_count; ++i) {
        const char *s = hist_index(i);
        if (!s) continue;
        int L = lcs_len(s, term);
        if (L > best_len) {
            best_len = L;
            cands_n = 0;
            cands[cands_n++] = i;
        } else if (L == best_len && L > 0) {
            cands[cands_n++] = i;
        }
    }

    if (best_len <= 2 || cands_n == 0) {
        const char *msg = "No match for search term in history\n";
        tabs_append_output(tab_idx, msg, (ssize_t)strlen(msg));
        free(cands);
        return;
    }

    /* Print candidates from most recent to older (so sort by index) */
    /* We'll output header */
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr), "No exact match. Best substring length = %d. Showing candidates:\n", best_len);
    tabs_append_output(tab_idx, hdr, hlen);

    /* print from most recent: indices increase from 0 (oldest) to hist_count-1 (newest) */
    for (int i = cands_n - 1; i >= 0; --i) {
        int hist_i = cands[i];
        const char *s = hist_index(hist_i);
        if (!s) continue;
        char out[4096];
        int L = snprintf(out, sizeof(out), "%s\n", s);
        tabs_append_output(tab_idx, out, L);
    }
    free(cands);
}
