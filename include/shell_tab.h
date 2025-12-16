#ifndef SHELL_TAB_H
#define SHELL_TAB_H

#include <pthread.h>
#include <sys/types.h>
#include <stddef.h>
#include "line_edit.h"

#define INPUT_MAX 8192

/* forward declare the LineEditor type (if you added line_edit.c) */
typedef struct LineEditor LineEditor;

typedef struct Tab {
    int id;
    pid_t pid;
    int to_child_fd;
    int from_child_fd;

    char input[INPUT_MAX];
    int input_len;
    int input_pos;

    char *out_buf;
    size_t out_len;
    size_t out_cap;

    int alive;

    pthread_mutex_t lock;

    /* optional pointer to LineEditor if you created it; may be NULL */
    LineEditor *editor;

    /* autocomplete state */
    int comp_pending;        /* 1 if choices are printed and awaiting numeric choice */
    char **comp_matches;     /* allocated array of printable basenames (malloc'd strings) */
    int comp_count;          /* how many stored (<= 9) */
    int comp_token_start;    /* original token start byte index */
    int comp_token_len;      /* original token length in bytes */
    char *comp_dir;          /* malloc'd directory prefix used for matching (or "./") */
} Tab;

/* functions in shell_tab.c */
int tabs_init(void);
int tabs_create(void);
int tabs_count(void);
Tab* tabs_get(int idx);
int tabs_get_fd(int idx);
void tabs_set_notify_fd(int fd);
void tabs_append_output(int idx, const char *buf, ssize_t n);
void tabs_read_once(int idx);
ssize_t tabs_write(int idx, const char *buf, size_t len);
void tabs_close(int idx);
void tabs_cleanup(void);


#endif /* SHELL_TAB_H */
