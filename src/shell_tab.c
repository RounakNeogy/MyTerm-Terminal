#define _POSIX_C_SOURCE 200809L
#include "shell_tab.h"
#include "line_edit.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

#define MAX_TABS 8
#define INITIAL_CAP 16384

/* Use pointer array so we can move pointers safely without copying mutex objects */
static Tab *tabs[MAX_TABS];
static int g_count = 0;

/* --- notify pipe write-end (optional) ---
   main() can call tabs_set_notify_fd(write_fd) to give us a write-end of a pipe.
   When we append new output we write one byte to wake the main loop's select().
*/
static int g_notify_fd = -1;

/* Allow main.c to give us a notify pipe write-end so we can wake the UI */
void tabs_set_notify_fd(int fd) {
    g_notify_fd = fd;
}


/* Helper: allocate and initialize a Tab object */
static Tab *tab_alloc(int id) {
    Tab *t = calloc(1, sizeof(Tab));
    if (!t) return NULL;
    t->id = id;
    t->pid = -1;
    t->to_child_fd = -1;
    t->from_child_fd = -1;
    t->input_len = 0;
    t->input[0] = '\0';
    t->input_pos = 0;        /* cursor index for line editing */
    t->editor = NULL;        /* created when tab is made */
    t->out_buf = NULL;
    t->out_len = 0;
    t->out_cap = 0;
    t->alive = 0;
    if (pthread_mutex_init(&t->lock, NULL) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

/* Helper: free tab (assumes mutex already unlocked and tab->alive cleaned up) */
static void tab_free(Tab *t) {
    if (!t) return;
    /* destroy associated editor if any */
    if (t->editor) {
        le_destroy(t->editor);
        t->editor = NULL;
    }
    pthread_mutex_destroy(&t->lock);
    if (t->out_buf) free(t->out_buf);
    free(t);
}

/* internal: ensure capacity for append; caller must hold lock */
static void ensure_capacity_locked(Tab *t, size_t extra) {
    if (!t) return;
    if (t->out_len + extra + 1 >= t->out_cap) {
        size_t nc = t->out_cap ? t->out_cap * 2 : INITIAL_CAP;
        while (nc < t->out_len + extra + 1) nc *= 2;
        char *p = realloc(t->out_buf, nc);
        if (!p) return;
        t->out_buf = p;
        t->out_cap = nc;
    }
}

/* thread-safe append exposed to other modules */
void tabs_append_output(int idx, const char *buf, ssize_t n) {
    if (idx < 0 || idx >= g_count) return;
    Tab *t = tabs[idx];
    if (!t) return;

    /* append under lock */
    pthread_mutex_lock(&t->lock);
    ensure_capacity_locked(t, (size_t)n);
    if (t->out_buf && n > 0) {
        memcpy(t->out_buf + t->out_len, buf, n);
        t->out_len += n;
        t->out_buf[t->out_len] = '\0';
    }
    pthread_mutex_unlock(&t->lock);

    /* Best-effort notify: write one byte into notify pipe (non-blocking should be set by caller)
       and otherwise rely on need_redraw flag in main loop to pull changes. */
    if (g_notify_fd >= 0) {
        ssize_t w;
        do {
            w = write(g_notify_fd, "x", 1);
        } while (w < 0 && errno == EINTR);
        /* ignore errors (EAGAIN, EPIPE, etc.) */
        (void)w;
    }
}

/* init tabs array */
int tabs_init(void) {
    g_count = 0;
    for (int i = 0; i < MAX_TABS; ++i) tabs[i] = NULL;
    return 0;
}

/* create a new tab using pipes and fork; child runs /bin/sh -s */
int tabs_create(void) {
    if (g_count >= MAX_TABS) return -1;
    int pipe_to[2];   /* parent -> child (stdin) */
    int pipe_from[2]; /* child -> parent (stdout+stderr) */
    if (pipe(pipe_to) < 0) return -1;
    if (pipe(pipe_from) < 0) { close(pipe_to[0]); close(pipe_to[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_to[0]); close(pipe_to[1]);
        close(pipe_from[0]); close(pipe_from[1]);
        return -1;
    }

    if (pid == 0) {
        /* child: connect pipes to stdin/stdout/stderr */
        close(pipe_to[1]);   /* close parent write end */
        close(pipe_from[0]); /* close parent read end */

        if (dup2(pipe_to[0], STDIN_FILENO) < 0) _exit(127);
        if (dup2(pipe_from[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(pipe_from[1], STDERR_FILENO) < 0) _exit(127);

        /* close unused fds */
        close(pipe_to[0]);
        close(pipe_from[1]);

        /* exec a simple shell */
        execlp("sh", "sh", "-s", NULL);
        _exit(127);
    }

    /* parent */
    close(pipe_to[0]);   /* close child read end */
    close(pipe_from[1]); /* close child write end */

    /* set non-blocking read on from-child fd so parent select/read is safe */
    int flags = fcntl(pipe_from[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipe_from[0], F_SETFL, flags | O_NONBLOCK);

    Tab *t = tab_alloc(g_count);
    if (!t) {
        close(pipe_to[1]);
        close(pipe_from[0]);
        /* ideally kill child */
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }

    t->pid = pid;
    t->to_child_fd = pipe_to[1];
    t->from_child_fd = pipe_from[0];
    t->out_buf = malloc(INITIAL_CAP);
    if (t->out_buf) t->out_cap = INITIAL_CAP; else t->out_cap = 0;
    t->out_len = 0;
    t->alive = 1;

    /* create a LineEditor for the tab for GUI integration (prompt can be set by GUI later).
       We pass NULL so the editor uses an empty prompt; GUI draws PROMPT itself. */
    t->editor = le_create(NULL);
    if (!t->editor) {
        /* non-fatal: proceed without editor */
        t->editor = NULL;
    }

    tabs[g_count] = t;
    g_count++;
    return t->id;
}

int tabs_count(void) { return g_count; }

Tab* tabs_get(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return tabs[idx];
}

int tabs_get_fd(int idx) {
    Tab *t = tabs_get(idx);
    if (!t) return -1;
    return t->from_child_fd;
}

/* read available data once (non-blocking); append to out_buf */
void tabs_read_once(int idx) {
    Tab *t = tabs_get(idx);
    if (!t || !t->alive) return;
    char buf[4096];
    ssize_t r;
    while ((r = read(t->from_child_fd, buf, sizeof(buf))) > 0) {
        /* use tabs_append_output to be safe even if reader called elsewhere */
        tabs_append_output(idx, buf, r);
    }
    if (r == 0) {
        /* EOF: child closed its end */
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "\n[process %d closed]\n", (int)t->pid);
        tabs_append_output(idx, msg, n);
        close(t->from_child_fd);
        close(t->to_child_fd);
        t->alive = 0;
        waitpid(t->pid, NULL, 0);
    } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        /* unrecoverable read error */
        char em[128];
        int n = snprintf(em, sizeof(em), "\n[read err: %s]\n", strerror(errno));
        tabs_append_output(idx, em, n);
    }
}

ssize_t tabs_write(int idx, const char *buf, size_t len) {
    Tab *t = tabs_get(idx);
    if (!t || !t->alive) return -1;
    ssize_t w = write(t->to_child_fd, buf, len);
    return w;
}

/* close a tab gracefully */
void tabs_close(int idx) {
    if (idx < 0 || idx >= g_count) return;
    Tab *t = tabs[idx];
    if (!t) return;
    if (t->alive) {
        const char *ex = "exit\n";
        write(t->to_child_fd, ex, strlen(ex));
        /* give it a moment */
        usleep(1000);
        int status;
        pid_t w = waitpid(t->pid, &status, WNOHANG);
        if (w == 0) {
            kill(t->pid, SIGTERM);
            waitpid(t->pid, &status, 0);
        }
        close(t->to_child_fd);
        close(t->from_child_fd);
        t->alive = 0;
    }

    /* free resources and destroy the tab object */
    tab_free(t);

    /* shift later tab pointers left and update their ids */
    for (int i = idx + 1; i < g_count; ++i) {
        tabs[i-1] = tabs[i];
        if (tabs[i-1]) tabs[i-1]->id = i-1;
    }
    tabs[g_count-1] = NULL;
    g_count--;
}

/* cleanup all tabs at exit */
void tabs_cleanup(void) {
    for (int i = 0; i < g_count; ++i) {
        Tab *t = tabs[i];
        if (!t) continue;
        if (t->alive) {
            close(t->to_child_fd);
            close(t->from_child_fd);
            kill(t->pid, SIGTERM);
            waitpid(t->pid, NULL, 0);
            t->alive = 0;
        }
        tab_free(t);
        tabs[i] = NULL;
    }
    g_count = 0;
}
