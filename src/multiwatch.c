#define _POSIX_C_SOURCE 200809L
#include "multiwatch.h"
#include "shell_tab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#define MW_POLL_MS 400

typedef struct mw_state {
    int tab_idx;
    int n;
    pid_t *pids;           /* child pids */
    char **cmds;           /* duplicated command strings */
    char **temp_paths;     /* ".temp.<pid>.txt" strings (created by child using getpid) */
    off_t *offsets;        /* offsets we last read from each temp file */
    int running;           /* 1 running, 0 stopping */
    pthread_t monitor_thr;
    int monitor_started;   /* 1 if monitor thread was successfully created */
    pthread_mutex_t lock;
    struct mw_state *next;
} mw_state;

/* global list of states (linked list) so we can find by tab_idx */
static mw_state *mw_list = NULL;
static pthread_mutex_t mw_list_lock = PTHREAD_MUTEX_INITIALIZER;

/* helper: current formatted timestamp into buffer (localtime) */
static void fmt_time_now(char *buf, size_t buflen) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, buflen, "%Y-%m-%d %H:%M:%S", &tm);
}

/* helper: create temp path from pid: ".temp.<pid>.txt" */
static char *make_temp_for_pid(pid_t pid) {
    char tbuf[64];
    int n = snprintf(tbuf, sizeof(tbuf), ".temp.%d.txt", (int)pid);
    if (n < 0) return NULL;
    return strdup(tbuf);
}

/* monitor thread: periodically read any newly appended bytes from temp files and append to tab output */
static void *monitor_thread_fn(void *v) {
    mw_state *s = (mw_state *)v;
    if (!s) return NULL;

    while (1) {
        pthread_mutex_lock(&s->lock);
        if (!s->running) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        pthread_mutex_unlock(&s->lock);

        for (int i = 0; i < s->n; ++i) {
            /* open temp file for reading; if not present yet, skip */
            char *path = s->temp_paths[i];
            if (!path) continue;
            FILE *f = fopen(path, "r");
            if (!f) continue; /* maybe child not created file yet */
            /* seek to last offset */
            if (s->offsets[i] > 0) {
                if (fseeko(f, s->offsets[i], SEEK_SET) != 0) {
                    /* maybe file truncated -> start at 0 */
                    fseeko(f, 0, SEEK_SET);
                    s->offsets[i] = 0;
                }
            }
            /* Read any new bytes (we read in a loop to gather all currently available) */
            char buf[4096];
            size_t got = 0;
            char *acc = NULL;
            size_t acc_sz = 0;
            while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
                char *tmp = realloc(acc, acc_sz + got);
                if (!tmp) { free(acc); acc = NULL; acc_sz = 0; break; }
                acc = tmp;
                memcpy(acc + acc_sz, buf, got);
                acc_sz += got;
            }
            off_t cur_off = ftello(f);
            if (cur_off >= 0) s->offsets[i] = cur_off;
            fclose(f);

            if (acc && acc_sz > 0) {
                /* Build formatted output: " "<cmd>"\ncurrent time: ...\n----\n<acc>\n----\n" */
                char timebuf[64];
                fmt_time_now(timebuf, sizeof(timebuf));
                char header[512];
                int hlen = snprintf(header, sizeof(header), "\n\"%s\"\ncurrent time: %s\n----------------------------------------------------\n", s->cmds[i], timebuf);
                tabs_append_output(s->tab_idx, header, hlen);
                tabs_append_output(s->tab_idx, acc, (ssize_t)acc_sz);
                const char *sep = "\n----------------------------------------------------\n";
                tabs_append_output(s->tab_idx, sep, (ssize_t)strlen(sep));
                free(acc);
            }
        }

        /* sleep for the poll interval */
        usleep(MW_POLL_MS * 1000);
    }

    /* after being asked to stop, wait for children to exit and report exit codes, then cleanup files */
    for (int i = 0; i < s->n; ++i) {
        pid_t pid = s->pids[i];
        if (pid > 0) {
            int status = 0;
            pid_t w = waitpid(pid, &status, 0);
            if (w > 0) {
                if (WIFEXITED(status)) {
                    int code = WEXITSTATUS(status);
                    char msg[256];
                    int n = snprintf(msg, sizeof(msg), "\n[%s exited with code %d]\n", s->cmds[i], code);
                    tabs_append_output(s->tab_idx, msg, n);
                } else if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    char msg[256];
                    int n = snprintf(msg, sizeof(msg), "\n[%s killed by signal %d]\n", s->cmds[i], sig);
                    tabs_append_output(s->tab_idx, msg, n);
                }
            }
        }
        /* remove temp file if exists */
        if (s->temp_paths[i]) unlink(s->temp_paths[i]);
    }

    return NULL;
}

/* find mw_state by tab_idx; caller must hold mw_list_lock if wants stable reference, we return pointer (not copied) */
static mw_state *mw_find_by_tab(int tab_idx) {
    mw_state *p = mw_list;
    while (p) {
        if (p->tab_idx == tab_idx) return p;
        p = p->next;
    }
    return NULL;
}

/* start multiwatch: spawn children and start monitor thread */
int multiwatch_start(int tab_idx, const char **cmds_in, int ncmds) {
    if (!cmds_in || ncmds <= 0) return -1;

    /* make a state */
    mw_state *s = calloc(1, sizeof(mw_state));
    if (!s) return -1;
    s->tab_idx = tab_idx;
    s->n = ncmds;
    s->pids = calloc(ncmds, sizeof(pid_t));
    s->cmds = calloc(ncmds, sizeof(char *));
    s->temp_paths = calloc(ncmds, sizeof(char *));
    s->offsets = calloc(ncmds, sizeof(off_t));
    if (!s->pids || !s->cmds || !s->temp_paths || !s->offsets) {
        free(s->pids); free(s->cmds); free(s->temp_paths); free(s->offsets); free(s);
        return -1;
    }
    for (int i = 0; i < ncmds; ++i) {
        s->cmds[i] = strdup(cmds_in[i] ? cmds_in[i] : "");
        s->temp_paths[i] = NULL;
        s->offsets[i] = 0;
        s->pids[i] = 0;
    }
    pthread_mutex_init(&s->lock, NULL);
    s->running = 1;
    s->monitor_started = 0; /* not started yet */

    /* add to global list */
    pthread_mutex_lock(&mw_list_lock);
    s->next = mw_list;
    mw_list = s;
    pthread_mutex_unlock(&mw_list_lock);

    /* spawn children — each child will redirect stdout/stderr to .temp.<pid>.txt and loop with trap for SIGINT */
    for (int i = 0; i < ncmds; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            /* fork error: kill earlier children and cleanup */
            for (int j = 0; j < i; ++j) {
                if (s->pids[j] > 0) kill(s->pids[j], SIGINT);
            }
            /* wait for them briefly (blocking) */
            for (int j = 0; j < i; ++j) {
                if (s->pids[j] > 0) waitpid(s->pids[j], NULL, 0);
            }
            multiwatch_interrupt(tab_idx);
            return -1;
        }
        if (pid == 0) {
            /* child */
            /* create its own temp file path based on its PID */
            pid_t mypid = getpid();
            char tmpname[64];
            snprintf(tmpname, sizeof(tmpname), ".temp.%d.txt", (int)mypid);

            /* open file for append (create/truncate on start) */
            int fd = open(tmpname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                /* redirect stdout & stderr to this file */
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2) close(fd);
            }

            /* Make sure child runs in its own process group (so sending signals can target set) */
            setpgid(0, 0);

            /* Build the shell string:
               trap 'exit' INT; while true; do <cmd>; sleep 1; done
               We use sh -c to interpret commands as entered by user.
            */
            const char *user_cmd = s->cmds[i];
            /* be careful with buffer size; just allocate */
            size_t bufsz = strlen(user_cmd) + 128;
            char *shcmd = malloc(bufsz);
            if (!shcmd) _exit(127);
            /* create command with trap & loop; redirecting is already done by fd dup2 */
            snprintf(shcmd, bufsz, "trap 'exit' INT; while true; do %s; sleep 1; done", user_cmd);
            execlp("sh", "sh", "-c", shcmd, (char*)NULL);
            /* if execlp fails */
            dprintf(STDERR_FILENO, "exec sh failed: %s\n", strerror(errno));
            free(shcmd);
            _exit(127);
        }

        /* parent */
        s->pids[i] = pid;
        /* ensure its temp path is constructed using the child's pid */
        s->temp_paths[i] = make_temp_for_pid(pid);
        /* set offset initially 0 */
        s->offsets[i] = 0;
        /* set its pgid (we want to make sure children in same PGID? leave separate — multiwatch_interrupt will send to each child individually) */
        /* setpgid in parent to ensure group set (ignore errors) */
        setpgid(pid, pid);
    }

    /* spawn monitor thread */
    if (pthread_create(&s->monitor_thr, NULL, monitor_thread_fn, s) != 0) {
        /* failed to create monitor: kill children and cleanup */
        for (int i = 0; i < s->n; ++i) if (s->pids[i] > 0) kill(s->pids[i], SIGINT);
        for (int i = 0; i < s->n; ++i) if (s->pids[i] > 0) waitpid(s->pids[i], NULL, 0);
        for (int i = 0; i < s->n; ++i) if (s->temp_paths[i]) unlink(s->temp_paths[i]);
        for (int i = 0; i < s->n; ++i) free(s->cmds[i]);
        free(s->cmds); free(s->pids); free(s->temp_paths); free(s->offsets);
        /* remove from list */
        pthread_mutex_lock(&mw_list_lock);
        if (mw_list == s) mw_list = s->next;
        else {
            mw_state *p = mw_list;
            while (p && p->next != s) p = p->next;
            if (p) p->next = s->next;
        }
        pthread_mutex_unlock(&mw_list_lock);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return -1;
    }
    /* mark thread started; do NOT detach - we'll join it in multiwatch_interrupt */
    s->monitor_started = 1;

    return 0;
}

/* interrupt: signal children and stop monitor. returns 0 on success */
int multiwatch_interrupt(int tab_idx) {
    pthread_mutex_lock(&mw_list_lock);
    mw_state *s = mw_find_by_tab(tab_idx);
    pthread_mutex_unlock(&mw_list_lock);
    if (!s) return -1;

    pthread_mutex_lock(&s->lock);
    if (!s->running) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    s->running = 0;
    /* send SIGINT to each child (use process group or direct pid) */
    for (int i = 0; i < s->n; ++i) {
        pid_t pid = s->pids[i];
        if (pid > 0) {
            /* send SIGINT to the process group of child (negative pid) to capture possible sub-processes */
            kill(-pid, SIGINT);
            /* also send to the pid in case group not set */
            kill(pid, SIGINT);
        }
    }
    pthread_mutex_unlock(&s->lock);

    /* If monitor thread was started, wait for it to finish (it will reap children & cleanup files). */
    if (s->monitor_started) {
        /* join the monitor thread (it will finish its final waitpid/unlink work) */
        pthread_join(s->monitor_thr, NULL);
    } else {
        /* Monitor thread not started (rare); main thread must wait for children itself */
        for (int i = 0; i < s->n; ++i) {
            pid_t pid = s->pids[i];
            if (pid > 0) {
                waitpid(pid, NULL, 0);
            }
        }
        /* and remove temp files (monitor would have done it otherwise) */
        for (int i = 0; i < s->n; ++i) {
            if (s->temp_paths[i]) unlink(s->temp_paths[i]);
        }
    }

    /* remove from global list */
    pthread_mutex_lock(&mw_list_lock);
    if (mw_list == s) mw_list = s->next;
    else {
        mw_state *p = mw_list;
        while (p && p->next != s) p = p->next;
        if (p) p->next = s->next;
    }
    pthread_mutex_unlock(&mw_list_lock);

    /* cleanup memory (monitor thread already used temp_paths and performed unlink; safe to free now) */
    for (int i = 0; i < s->n; ++i) {
        free(s->temp_paths[i]);
        free(s->cmds[i]);
    }
    free(s->temp_paths);
    free(s->cmds);
    free(s->pids);
    free(s->offsets);
    pthread_mutex_destroy(&s->lock);
    free(s);

    const char *msg = "\n[multiwatch stopped successfully]\n";
    tabs_append_output(tab_idx, msg, strlen(msg));

    return 0;
}
