#define _POSIX_C_SOURCE 200809L
#include "cmd_exec.h"
#include "shell_tab.h"
#include "history.h"
#include "multiwatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <glob.h>
#include <strings.h>



/* Maximum tabs we track for PGID mapping. Keep in sync with your tabs_count limit. */
#ifndef CMD_MAX_TABS
#define CMD_MAX_TABS 64
#endif

#ifndef GLOB_TILDE
#define GLOB_TILDE 0
#endif


/* -------------------- PGID tracking -------------------- */
/* Per-tab foreground process group id. Protected by pgid_lock. */
static pid_t tab_pgid[CMD_MAX_TABS];
static pthread_mutex_t pgid_lock = PTHREAD_MUTEX_INITIALIZER;

/* helper: set PGID for a tab */
static void set_tab_pgid(int tab_idx, pid_t pgid)
{
    if (tab_idx < 0 || tab_idx >= CMD_MAX_TABS)
        return;
    pthread_mutex_lock(&pgid_lock);
    tab_pgid[tab_idx] = pgid;
    pthread_mutex_unlock(&pgid_lock);
}

/* helper: get PGID for a tab */
static pid_t get_tab_pgid(int tab_idx)
{
    pid_t r = 0;
    if (tab_idx < 0 || tab_idx >= CMD_MAX_TABS)
        return 0;
    pthread_mutex_lock(&pgid_lock);
    r = tab_pgid[tab_idx];
    pthread_mutex_unlock(&pgid_lock);
    return r;
}

/* Clear PGID for a tab */
static void clear_tab_pgid(int tab_idx)
{
    set_tab_pgid(tab_idx, 0);
}

/* -------------------- reader thread + execution code -------------------- */

/* Reader thread args can wait for multiple children */
struct reader_args
{
    int tab_idx;
    int fd;          /* read end to capture */
    pid_t *children; /* array of child pids */
    int child_count;
};

/* reader thread: read everything from fd and then wait for children.
   After children finish (or stop), it reports statuses and clears tab PGID. */
static void *reader_thread(void *v)
{
    struct reader_args *a = v;
    int tab = a->tab_idx;
    int fd = a->fd;
    pid_t *kids = a->children;
    int n = a->child_count;

    char buf[4096];
    ssize_t r;
    /* read all available output until EOF */
    while ((r = read(fd, buf, sizeof(buf))) > 0)
    {
        tabs_append_output(tab, buf, r);
    }
    /* close the read end (children still exist) */
    if (fd >= 0)
        close(fd);

    /* wait for each child (detect stopped/exited). We'll first use WUNTRACED
       to learn if a child was stopped (SIGTSTP). Then, if stopped, we still
       wait for its final exit (blocking wait) to report eventual exit. */
    for (int i = 0; i < n; ++i)
    {
        int status = 0;
        pid_t w = waitpid(kids[i], &status, WUNTRACED);
        if (w <= 0)
            continue;
        if (WIFEXITED(status))
        {
            int code = WEXITSTATUS(status);
            char msg[128];
            int m = snprintf(msg, sizeof(msg), "\n[process %d exited with status %d]\n", (int)kids[i], code);
            tabs_append_output(tab, msg, m);
        }
        else if (WIFSIGNALED(status))
        {
            int sig = WTERMSIG(status);
            char msg[128];
            int m = snprintf(msg, sizeof(msg), "\n[process %d killed by signal %d]\n", (int)kids[i], sig);
            tabs_append_output(tab, msg, m);
        }
        else if (WIFSTOPPED(status))
        {
            int sig = WSTOPSIG(status);
            char msg[128];
            int m = snprintf(msg, sizeof(msg), "\n[process %d stopped by signal %d]\n", (int)kids[i], sig);
            tabs_append_output(tab, msg, m);
            /* now wait for eventual termination (block) and then report that too */
            int status2 = 0;
            waitpid(kids[i], &status2, 0);
            if (WIFEXITED(status2))
            {
                int code = WEXITSTATUS(status2);
                int m2 = snprintf(msg, sizeof(msg), "\n[process %d exited with status %d]\n", (int)kids[i], code);
                tabs_append_output(tab, msg, m2);
            }
            else if (WIFSIGNALED(status2))
            {
                int sig2 = WTERMSIG(status2);
                int m2 = snprintf(msg, sizeof(msg), "\n[process %d killed by signal %d]\n", (int)kids[i], sig2);
                tabs_append_output(tab, msg, m2);
            }
        }
        else
        {
            char msg[128];
            int m = snprintf(msg, sizeof(msg), "\n[process %d ended]\n", (int)kids[i]);
            tabs_append_output(tab, msg, m);
        }
    }

    /* clear PGID mapping for this tab (foreground job done/stopped) */
    clear_tab_pgid(tab);

    free(kids);
    free(a);
    return NULL;
}

/* -------------------- tokenization / quoting / glob helpers -------------------- */
/* (I reuse tokenizer + glob expansion logic similar to your prior implementation) */

/* Preprocess: insert spaces around '|' outside quotes so tokenizer sees '|' as separate token */
static char *preprocess_pipes(const char *in)
{
    size_t n = strlen(in);
    char *out = malloc(n * 3 + 1); /* generous */
    if (!out)
        return NULL;
    const char *p = in;
    char *q = out;
    int in_single = 0, in_double = 0;
    while (*p)
    {
        if (*p == '\'' && !in_double)
        {
            in_single = !in_single;
            *q++ = *p++;
            continue;
        }
        if (*p == '"' && !in_single)
        {
            in_double = !in_double;
            *q++ = *p++;
            continue;
        }
        if (!in_single && !in_double && *p == '|')
        {
            *q++ = ' ';
            *q++ = '|';
            *q++ = ' ';
            ++p;
            continue;
        }
        *q++ = *p++;
    }
    *q = '\0';
    return out;
}

/* simple quoted tokenizer (strdup's tokens) */
static int tokenize_quoted(const char *input, char **argv, int max_args)
{
    int argc = 0;
    const char *p = input;
    while (*p && argc < max_args - 1)
    {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n'))
            ++p;
        if (!*p)
            break;
        int in_single = 0, in_double = 0;
        char tmp[8192];
        int ti = 0;
        while (*p)
        {
            if (!in_single && *p == '"')
            {
                in_double = !in_double;
                ++p;
                continue;
            }
            if (!in_double && *p == '\'')
            {
                in_single = !in_single;
                ++p;
                continue;
            }
            if (!in_single && !in_double && (*p == ' ' || *p == '\t' || *p == '\n'))
                break;
            if (*p == '\\')
            {
                ++p;
                if (!*p)
                    break;
                if (*p == 'n')
                    tmp[ti++] = '\n';
                else if (*p == 't')
                    tmp[ti++] = '\t';
                else if (*p == '\\')
                    tmp[ti++] = '\\';
                else if (*p == '"')
                    tmp[ti++] = '"';
                else if (*p == '\'')
                    tmp[ti++] = '\'';
                else
                    tmp[ti++] = *p;
                ++p;
                continue;
            }
            tmp[ti++] = *p++;
            if (ti >= (int)sizeof(tmp) - 2)
                break;
        }
        tmp[ti] = '\0';
        argv[argc] = strdup(tmp);
        if (!argv[argc])
        {
            for (int i = 0; i < argc; ++i)
                free(argv[i]);
            return -1;
        }
        argc++;
    }
    argv[argc] = NULL;
    return argc;
}

/* glob helpers */
static int is_operator_token(const char *tok)
{
    if (!tok)
        return 0;
    if (strcmp(tok, "|") == 0)
        return 1;
    if (strcmp(tok, "<") == 0)
        return 1;
    if (strcmp(tok, ">") == 0)
        return 1;
    if (strcmp(tok, ">>") == 0)
        return 1;
    if (tok[0] == '<' || tok[0] == '>')
        return 1;
    return 0;
}
static int contains_glob_chars(const char *s)
{
    if (!s)
        return 0;
    for (; *s; ++s)
    {
        if (*s == '*' || *s == '?' || *s == '[')
            return 1;
    }
    return 0;
}
static char **expand_tokens_with_glob(char **tokens, int ntoks, int *out_ntoks)
{
    if (!tokens || ntoks <= 0)
    {
        *out_ntoks = 0;
        return NULL;
    }
    int cap = ntoks * 4 + 16;
    char **newt = malloc(sizeof(char *) * cap);
    if (!newt)
    {
        *out_ntoks = 0;
        return NULL;
    }
    int n = 0;
    for (int i = 0; i < ntoks; ++i)
    {
        char *tk = tokens[i];
        if (!tk)
            continue;
        if (is_operator_token(tk) || !contains_glob_chars(tk))
        {
            newt[n++] = strdup(tk);
            continue;
        }
        glob_t g;
        memset(&g, 0, sizeof(g));
        int flags = GLOB_NOCHECK | GLOB_TILDE;
        int rc = glob(tk, flags, NULL, &g);
        if (rc == 0)
        {
            for (size_t j = 0; j < g.gl_pathc; ++j)
            {
                if (n + 1 >= cap)
                {
                    cap *= 2;
                    char **tmp = realloc(newt, sizeof(char *) * cap);
                    if (!tmp)
                        break;
                    newt = tmp;
                }
                newt[n++] = strdup(g.gl_pathv[j]);
            }
        }
        else
        {
            newt[n++] = strdup(tk);
        }
        globfree(&g);
    }
    *out_ntoks = n;
    char **final = realloc(newt, sizeof(char *) * (n + 1));
    if (!final)
        final = newt;
    final[n] = NULL;
    return final;
}
static void free_tokens(char **tokens, int ntoks)
{
    if (!tokens)
        return;
    for (int i = 0; i < ntoks; ++i)
        if (tokens[i])
            free(tokens[i]);
    free(tokens);
}

/* -------------------- command parse/execution -------------------- */

/* Simple Cmd structure */
typedef struct
{
    char **argv; /* NULL-terminated */
    int argc;
    char *infile;  /* optional (malloc'd) */
    char *outfile; /* optional (malloc'd) */
    int append;    /* for outfile: append if 1 */
} Cmd;
static void free_cmd(Cmd *c)
{
    if (!c)
        return;
    if (c->argv)
    {
        for (int i = 0; i < c->argc; ++i)
            if (c->argv[i])
                free(c->argv[i]);
        free(c->argv);
    }
    if (c->infile)
        free(c->infile);
    if (c->outfile)
        free(c->outfile);
    c->argv = NULL;
    c->argc = 0;
    c->infile = NULL;
    c->outfile = NULL;
    c->append = 0;
}

static void sanitize_filename(char *s)
{
    if (!s)
        return;
    char *start = s;
    while (*start && isspace((unsigned char)*start))
        ++start;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    size_t L = strlen(s);
    while (L > 0 && (isspace((unsigned char)s[L - 1]) || s[L - 1] == '\r'))
    {
        s[L - 1] = '\0';
        --L;
    }
}

/* parse tokens into Cmds (handles | and < > >>) */
static int parse_tokens_into_cmds(char **tokens, int ntoks, Cmd *cmds, int max_cmds)
{
    int cmd_idx = 0;
    if (max_cmds < 1)
        return -1;
    for (int z = 0; z < max_cmds; ++z)
    {
        cmds[z].argv = NULL;
        cmds[z].argc = 0;
        cmds[z].infile = NULL;
        cmds[z].outfile = NULL;
        cmds[z].append = 0;
    }
    cmds[cmd_idx].argc = 0;
    cmds[cmd_idx].argv = malloc(sizeof(char *) * 128);
    if (!cmds[cmd_idx].argv)
        return -1;
    int argcap = 128;
    for (int i = 0; i < ntoks; ++i)
    {
        char *tk = tokens[i];
        if (!tk)
            continue;
        if (strcmp(tk, "|") == 0)
        {
            cmds[cmd_idx].argv[cmds[cmd_idx].argc] = NULL;
            cmd_idx++;
            if (cmd_idx >= max_cmds)
                return -1;
            cmds[cmd_idx].argc = 0;
            cmds[cmd_idx].argv = malloc(sizeof(char *) * 128);
            if (!cmds[cmd_idx].argv)
                return -1;
            argcap = 128;
            continue;
        }
        if (strcmp(tk, "<") == 0)
        {
            if (i + 1 >= ntoks)
                return -1;
            if (cmds[cmd_idx].infile)
                free(cmds[cmd_idx].infile);
            cmds[cmd_idx].infile = strdup(tokens[++i]);
            if (cmds[cmd_idx].infile)
                sanitize_filename(cmds[cmd_idx].infile);
            continue;
        }
        else if (tk[0] == '<' && tk[1] != '\0')
        {
            if (cmds[cmd_idx].infile)
                free(cmds[cmd_idx].infile);
            cmds[cmd_idx].infile = strdup(tk + 1);
            if (cmds[cmd_idx].infile)
                sanitize_filename(cmds[cmd_idx].infile);
            continue;
        }
        else if (strcmp(tk, ">>") == 0)
        {
            if (i + 1 >= ntoks)
                return -1;
            if (cmds[cmd_idx].outfile)
                free(cmds[cmd_idx].outfile);
            cmds[cmd_idx].outfile = strdup(tokens[++i]);
            cmds[cmd_idx].append = 1;
            if (cmds[cmd_idx].outfile)
                sanitize_filename(cmds[cmd_idx].outfile);
            continue;
        }
        else if (strncmp(tk, ">>", 2) == 0)
        {
            if (cmds[cmd_idx].outfile)
                free(cmds[cmd_idx].outfile);
            cmds[cmd_idx].outfile = strdup(tk + 2);
            cmds[cmd_idx].append = 1;
            if (cmds[cmd_idx].outfile)
                sanitize_filename(cmds[cmd_idx].outfile);
            continue;
        }
        else if (strcmp(tk, ">") == 0)
        {
            if (i + 1 >= ntoks)
                return -1;
            if (cmds[cmd_idx].outfile)
                free(cmds[cmd_idx].outfile);
            cmds[cmd_idx].outfile = strdup(tokens[++i]);
            cmds[cmd_idx].append = 0;
            if (cmds[cmd_idx].outfile)
                sanitize_filename(cmds[cmd_idx].outfile);
            continue;
        }
        else if (tk[0] == '>' && tk[1] != '\0')
        {
            if (cmds[cmd_idx].outfile)
                free(cmds[cmd_idx].outfile);
            cmds[cmd_idx].outfile = strdup(tk + 1);
            cmds[cmd_idx].append = 0;
            if (cmds[cmd_idx].outfile)
                sanitize_filename(cmds[cmd_idx].outfile);
            continue;
        }
        if (cmds[cmd_idx].argc + 2 >= argcap)
        {
            argcap *= 2;
            char **newv = realloc(cmds[cmd_idx].argv, sizeof(char *) * argcap);
            if (!newv)
                return -1;
            cmds[cmd_idx].argv = newv;
        }
        cmds[cmd_idx].argv[cmds[cmd_idx].argc++] = strdup(tk);
    }
    cmds[cmd_idx].argv[cmds[cmd_idx].argc] = NULL;
    return cmd_idx + 1;
}

/* -------------------- Public: run command line -------------------- */
int cmd_exec_run_in_tab(int tab_idx, const char *cmdline)
{
    if (!cmdline)
        return -1;

    history_add(cmdline);

    char *pre = preprocess_pipes(cmdline);
    if (!pre)
        return -1;

    /* ---- Special parsing for multiwatch array syntax:
    Accept commands like:
    multiwatch ["cmd1","cmd2", ...]
    We'll parse quoted strings inside the [...] directly from cmdline
    (so embedded spaces are preserved). */
    do
    {
        /* quick check: does the cmdline start with "multiwatch" (case-insensitive) ? */
        const char *p0 = cmdline;
        while (*p0 && isspace((unsigned char)*p0))
            ++p0;
        size_t mwlen = strlen("multiwatch");
        if (strncasecmp(p0, "multiwatch", mwlen) == 0)
        {
            const char *p = p0 + mwlen;
            /* skip whitespace */
            while (*p && isspace((unsigned char)*p))
                ++p;
            /* Expect a '[' next (or error) */
            if (*p != '[')
                break; /* not the array form -> fall back to normal parsing */

            /* Scan inside brackets and extract double-quoted substrings */
            const char *endb = strchr(p, ']');
            if (!endb)
            {
                const char *msg = "multiwatch: malformed bracketed list (missing ])\n";
                tabs_append_output(tab_idx, msg, (ssize_t)strlen(msg));
                return -1;
            }

            /* We'll collect up to a reasonable limit (e.g. 128) */
            int max_cmds = 128;
            char **cmds = malloc(sizeof(char *) * max_cmds);
            if (!cmds)
                break;
            int cmd_count = 0;

            /* parser: find each double-quoted string between p and endb */
            const char *q = p;
            while (q && q < endb)
            {
                /* find next double quote */
                const char *open = strchr(q, '"');
                if (!open || open >= endb)
                    break;
                /* extract until next unescaped double quote */
                const char *r = open + 1;
                char tmp[8192];
                int ti = 0;
                while (r < endb)
                {
                    if (*r == '\\' && (r + 1) < endb)
                    {
                        /* simple escape handling: \n \t \\ \" etc */
                        ++r;
                        if (*r == 'n')
                            tmp[ti++] = '\n';
                        else if (*r == 't')
                            tmp[ti++] = '\t';
                        else
                            tmp[ti++] = *r;
                        ++r;
                        continue;
                    }
                    if (*r == '"')
                    { /* closing quote */
                        ++r;
                        break;
                    }
                    tmp[ti++] = *r++;
                    if (ti >= (int)sizeof(tmp) - 1)
                        break;
                }
                tmp[ti] = '\0';
                if (cmd_count < max_cmds)
                {
                    cmds[cmd_count] = strdup(tmp);
                    if (!cmds[cmd_count])
                    {
                        /* cleanup */
                        for (int j = 0; j < cmd_count; ++j)
                            free(cmds[j]);
                        free(cmds);
                        cmds = NULL;
                        break;
                    }
                    cmd_count++;
                }
                q = r;
                /* skip whitespace and optional commas */
                while (q < endb && (isspace((unsigned char)*q) || *q == ','))
                    ++q;
            }

            if (!cmds)
                break;

            if (cmd_count == 0)
            {
                const char *msg = "multiwatch: no commands found in list\n";
                tabs_append_output(tab_idx, msg, (ssize_t)strlen(msg));
                for (int j = 0; j < cmd_count; ++j)
                    free(cmds[j]);
                free(cmds);
                return -1;
            }

            /* call multiwatch_start (assumed to copy/duplicate commands if it needs them) */
            int rc = multiwatch_start(tab_idx, (const char **)cmds, cmd_count);

            /* free our duplicates (multiwatch_start should duplicate if it needs to keep them) */
            for (int j = 0; j < cmd_count; ++j)
                free(cmds[j]);
            free(cmds);

            return (rc == 0) ? 0 : -1;
        }
    } while (0);

    char *tokens_raw[512];
    int ntoks_raw = tokenize_quoted(pre, tokens_raw, (int)(sizeof(tokens_raw) / sizeof(tokens_raw[0])));
    free(pre);
    if (ntoks_raw <= 0)
    {
        for (int i = 0; i < ntoks_raw; ++i)
            if (tokens_raw[i])
                free(tokens_raw[i]);
        return -1;
    }

    int ntoks = 0;
    char **tokens = expand_tokens_with_glob(tokens_raw, ntoks_raw, &ntoks);
    for (int i = 0; i < ntoks_raw; ++i)
        if (tokens_raw[i])
            free(tokens_raw[i]);

    if (!tokens || ntoks <= 0)
    {
        if (tokens)
            free_tokens(tokens, ntoks);
        return -1;
    }

    const int MAX_CMDS = 64;
    Cmd cmds[MAX_CMDS];
    memset(cmds, 0, sizeof(cmds));
    int ncmds = parse_tokens_into_cmds(tokens, ntoks, cmds, MAX_CMDS);
    free_tokens(tokens, ntoks);
    if (ncmds <= 0)
    {
        for (int i = 0; i < MAX_CMDS; ++i)
            free_cmd(&cmds[i]);
        return -1;
    }

    /* builtin: cd */
    if (ncmds == 1 && cmds[0].argc > 0)
    {
        if (strcmp(cmds[0].argv[0], "cd") == 0)
        {
            const char *target = NULL;
            if (cmds[0].argc >= 2)
                target = cmds[0].argv[1];
            if (!target || strlen(target) == 0)
                target = getenv("HOME");
            if (!target)
            {
                const char *msg = "cd: no $HOME set\n";
                tabs_append_output(tab_idx, msg, strlen(msg));
                for (int i = 0; i < ncmds; ++i)
                    free_cmd(&cmds[i]);
                return -1;
            }
            if (chdir(target) == 0)
            {
                char msg[256];
                int n = snprintf(msg, sizeof(msg), "changed directory to %s\n", target);
                tabs_append_output(tab_idx, msg, n);
                for (int i = 0; i < ncmds; ++i)
                    free_cmd(&cmds[i]);
                return 0;
            }
            else
            {
                char err[256];
                int n = snprintf(err, sizeof(err), "cd: %s: %s\n", target, strerror(errno));
                tabs_append_output(tab_idx, err, n);
                for (int i = 0; i < ncmds; ++i)
                    free_cmd(&cmds[i]);
                return -1;
            }
        }
        else if (strcmp(cmds[0].argv[0], "history") == 0)
        {
            history_show_recent(tab_idx, 1000);
            return 0;
        }
    }

    /* open redirections early */
    int *in_fds = calloc(ncmds, sizeof(int));
    int *out_fds = calloc(ncmds, sizeof(int));
    if (!in_fds || !out_fds)
    {
        free(in_fds);
        free(out_fds);
        for (int i = 0; i < ncmds; ++i)
            free_cmd(&cmds[i]);
        return -1;
    }
    for (int i = 0; i < ncmds; ++i)
    {
        in_fds[i] = -1;
        out_fds[i] = -1;
        if (cmds[i].infile)
        {
            in_fds[i] = open(cmds[i].infile, O_RDONLY);
            if (in_fds[i] < 0)
            {
                char err[256];
                int m = snprintf(err, sizeof(err), "cannot open '%s' for reading: %s\n", cmds[i].infile, strerror(errno));
                tabs_append_output(tab_idx, err, m);
                for (int j = 0; j < ncmds; ++j)
                {
                    if (in_fds[j] >= 0)
                        close(in_fds[j]);
                    if (out_fds[j] >= 0)
                        close(out_fds[j]);
                    free_cmd(&cmds[j]);
                }
                free(in_fds);
                free(out_fds);
                return -1;
            }
        }
        if (cmds[i].outfile)
        {
            int flags = O_WRONLY | O_CREAT | (cmds[i].append ? O_APPEND : O_TRUNC);
            out_fds[i] = open(cmds[i].outfile, flags, 0644);
            if (out_fds[i] < 0)
            {
                char err[256];
                int m = snprintf(err, sizeof(err), "cannot open '%s' for writing: %s\n", cmds[i].outfile, strerror(errno));
                tabs_append_output(tab_idx, err, m);
                for (int j = 0; j < ncmds; ++j)
                {
                    if (in_fds[j] >= 0)
                        close(in_fds[j]);
                    if (out_fds[j] >= 0)
                        close(out_fds[j]);
                    free_cmd(&cmds[j]);
                }
                free(in_fds);
                free(out_fds);
                return -1;
            }
        }
    }

    /* create chain pipes and capture pipe */
    int chain_cnt = (ncmds > 1) ? ncmds - 1 : 0;
    int (*chain)[2] = NULL;
    if (chain_cnt > 0)
    {
        chain = malloc(sizeof(int[2]) * chain_cnt);
        if (!chain)
        {
            for (int j = 0; j < ncmds; ++j)
                free_cmd(&cmds[j]);
            free(in_fds);
            free(out_fds);
            return -1;
        }
        for (int i = 0; i < chain_cnt; ++i)
        {
            if (pipe(chain[i]) < 0)
            {
                for (int k = 0; k < i; ++k)
                {
                    close(chain[k][0]);
                    close(chain[k][1]);
                }
                free(chain);
                for (int j = 0; j < ncmds; ++j)
                    free_cmd(&cmds[j]);
                free(in_fds);
                free(out_fds);
                return -1;
            }
        }
    }

    int capture_pipe[2];
    if (pipe(capture_pipe) < 0)
    {
        for (int i = 0; i < chain_cnt; ++i)
        {
            close(chain[i][0]);
            close(chain[i][1]);
        }
        free(chain);
        for (int j = 0; j < ncmds; ++j)
            free_cmd(&cmds[j]);
        free(in_fds);
        free(out_fds);
        return -1;
    }

    pid_t *pids = calloc(ncmds, sizeof(pid_t));
    if (!pids)
    {
        for (int i = 0; i < chain_cnt; ++i)
        {
            close(chain[i][0]);
            close(chain[i][1]);
        }
        close(capture_pipe[0]);
        close(capture_pipe[1]);
        free(chain);
        for (int j = 0; j < ncmds; ++j)
            free_cmd(&cmds[j]);
        free(in_fds);
        free(out_fds);
        return -1;
    }

    /* Fork each command in the pipeline */
    for (int i = 0; i < ncmds; ++i)
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            for (int k = 0; k < i; ++k)
                kill(pids[k], SIGTERM);
            for (int k = 0; k < chain_cnt; ++k)
            {
                close(chain[k][0]);
                close(chain[k][1]);
            }
            close(capture_pipe[0]);
            close(capture_pipe[1]);
            free(chain);
            free(pids);
            for (int j = 0; j < ncmds; ++j)
                free_cmd(&cmds[j]);
            for (int j = 0; j < ncmds; ++j)
            {
                if (in_fds[j] >= 0)
                    close(in_fds[j]);
                if (out_fds[j] >= 0)
                    close(out_fds[j]);
            }
            free(in_fds);
            free(out_fds);
            return -1;
        }

        if (pid == 0)
        {
            /* child */
            /* setpgid: make every child in the same process group.
               We set child's pgid in parent too, but setting here is safe:
               leader will set its pgid to its pid (see parent code too) */
            if (i == 0)
            {
                /* leader - setpgid to own pid */
                setpgid(0, 0); /* setpgid(0,0) sets pgid = pid */
            }
            else
            {
                /* for other children, attempt to join leader later (parent also sets) */
            }

            if (in_fds[i] >= 0)
            {
                if (dup2(in_fds[i], STDIN_FILENO) < 0)
                    _exit(127);
            }
            else if (i > 0)
            {
                if (dup2(chain[i - 1][0], STDIN_FILENO) < 0)
                    _exit(127);
            }
            if (out_fds[i] >= 0)
            {
                if (dup2(out_fds[i], STDOUT_FILENO) < 0)
                    _exit(127);
            }
            else if (i < ncmds - 1)
            {
                if (dup2(chain[i][1], STDOUT_FILENO) < 0)
                    _exit(127);
            }
            else
            {
                if (dup2(capture_pipe[1], STDOUT_FILENO) < 0)
                    _exit(127);
            }
            if (dup2(capture_pipe[1], STDERR_FILENO) < 0)
                _exit(127);

            for (int k = 0; k < chain_cnt; ++k)
            {
                close(chain[k][0]);
                close(chain[k][1]);
            }
            close(capture_pipe[0]);
            close(capture_pipe[1]);

            for (int j = 0; j < ncmds; ++j)
            {
                if (in_fds[j] >= 0)
                    close(in_fds[j]);
                if (out_fds[j] >= 0)
                    close(out_fds[j]);
            }

            execvp(cmds[i].argv[0], cmds[i].argv);
            dprintf(STDERR_FILENO, "execvp failed: %s\n", strerror(errno));
            _exit(127);
        }

        /* parent records pid */
        pids[i] = pid;

        /* parent sets pgid for children so that all are in same group, leader = pids[0] */
        if (i == 0)
        {
            /* set leader pgid to leader pid */
            if (setpgid(pid, pid) < 0)
            {
                /* ignore error in typical race; try again */
                setpgid(pid, pid);
            }
        }
        else
        {
            if (setpgid(pid, pids[0]) < 0)
            {
                /* ignore */
                setpgid(pid, pids[0]);
            }
        }
    }

    /* Parent: close chain fds and capture write end */
    for (int k = 0; k < chain_cnt; ++k)
    {
        close(chain[k][0]);
        close(chain[k][1]);
    }
    free(chain);
    close(capture_pipe[1]);

    for (int j = 0; j < ncmds; ++j)
    {
        if (in_fds[j] >= 0)
            close(in_fds[j]);
        if (out_fds[j] >= 0)
            close(out_fds[j]);
    }
    free(in_fds);
    free(out_fds);

    /* set the tab PGID to the pipeline leader (pids[0]) so main can send signals */
    if (tab_idx >= 0 && tab_idx < CMD_MAX_TABS && pids[0] > 0)
    {
        set_tab_pgid(tab_idx, pids[0]);
    }

    /* Spawn reader thread */
    struct reader_args *ra = malloc(sizeof(*ra));
    if (!ra)
    {
        free(pids);
        close(capture_pipe[0]);
        for (int j = 0; j < ncmds; ++j)
            free_cmd(&cmds[j]);
        return 0;
    }
    ra->tab_idx = tab_idx;
    ra->fd = capture_pipe[0];
    ra->children = pids;
    ra->child_count = ncmds;

    pthread_t thr;
    if (pthread_create(&thr, NULL, reader_thread, ra) != 0)
    {
        close(capture_pipe[0]);
        free(ra->children);
        free(ra);
        for (int j = 0; j < ncmds; ++j)
            free_cmd(&cmds[j]);
        return 0;
    }
    pthread_detach(thr);

    for (int j = 0; j < ncmds; ++j)
        free_cmd(&cmds[j]);

    return 0;
}

/* -------------------- Public: interrupt / suspend -------------------- */

int cmd_exec_interrupt_tab(int tab_idx)
{
    pid_t pg = get_tab_pgid(tab_idx);
    if (pg <= 0)
        return -1;
    /* send SIGINT to process group (negative pid) */
    if (kill(-pg, SIGINT) < 0)
    {
        return -1;
    }
    return 0;
}

int cmd_exec_suspend_tab(int tab_idx)
{
    pid_t pg = get_tab_pgid(tab_idx);
    if (pg <= 0)
        return -1;
    if (kill(-pg, SIGTSTP) < 0)
    {
        return -1;
    }
    /* note: PGID remains mapped — reader thread will detect stopped/exit later and clear mapping */
    return 0;
}
