#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xlocale.h>

#include "shell_tab.h"
#include "cmd_exec.h"
#include "multiwatch.h"
#include "line_edit.h"
#include "history.h"
#include "autocomplete.h"

#define PROMPT "rounak@goatedterm> "
static Display *dpy = NULL;
static Window win;
static GC gc;
static XFontStruct *fontinfo = NULL;
static XFontSet fontset = NULL;
static int win_w = 900, win_h = 600;
static int line_height = 16;
static XIM xim = NULL;
static XIC xic = NULL;

static int active = -1;
/* background reader threads set this to request a GUI redraw */
volatile sig_atomic_t need_redraw = 0;

// ✅ Add this safe signal flag (used instead of calling multiwatch directly)
volatile sig_atomic_t interrupt_flag = 0;

// ✅ Minimal signal handler (async-signal-safe)
static void sigint_handler(int sig)
{
    (void)sig;
    interrupt_flag = 1; // Just set a flag, no heavy work here
}

// ✅ Setup function to install it
static void setup_signal_handlers(void)
{
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

/* We keep notify_pipe local t#ifndef GLOB_TILDE
#define GLOB_TILDE 0
#endifo main but visible inside main's loop.
   tabs_set_notify_fd(write_end) is called so other modules can notify main. */
static int notify_pipe_read = -1;
static int notify_pipe_write = -1;

static void die(const char *s)
{
    perror(s);
    history_save();
    tabs_cleanup();
    if (xic)
    {
        XDestroyIC(xic);
        xic = NULL;
    }
    if (xim)
    {
        XCloseIM(xim);
        xim = NULL;
    }
    if (dpy)
    {
        if (fontset)
            XFreeFontSet(dpy, fontset);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
    }
    exit(EXIT_FAILURE);
}

/* ---------- helper: detect if input has an unclosed quote ---------- */
static int input_has_unclosed_quote_buf(const char *s, size_t n)
{
    int in_single = 0, in_double = 0;
    char prev = 0;
    for (size_t i = 0; i < n; ++i)
    {
        char c = s[i];
        if (c == '\'' && !in_double && prev != '\\')
            in_single = !in_single;
        else if (c == '"' && !in_single && prev != '\\')
            in_double = !in_double;
        prev = c;
    }
    return (in_single || in_double);
}

/* ---------- helper: get clipboard text via X11 selection (synchronous) ---------- */
static char *get_clipboard_text(Display *d)
{
    if (!d)
        return NULL;
    Atom clip = XInternAtom(d, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
    Atom prop = XInternAtom(d, "MY_TERM_CLIP", False);
    Window owner = XGetSelectionOwner(d, clip);
    if (owner == None)
    {
        owner = XGetSelectionOwner(d, XA_PRIMARY);
        if (owner == None)
            return NULL;
    }

    XConvertSelection(d, clip, utf8, prop, win, CurrentTime);

    XEvent ev;
    time_t start = time(NULL);
    while (1)
    {
        if (XCheckTypedWindowEvent(d, win, SelectionNotify, &ev))
        {
            XSelectionEvent *sev = (XSelectionEvent *)&ev;
            if (sev->property == None)
            {
                return NULL;
            }
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *data = NULL;
            int rc = XGetWindowProperty(d, win, prop, 0, (~0L), False, AnyPropertyType,
                                        &actual_type, &actual_format, &nitems, &bytes_after, &data);
            if (rc != Success || data == NULL)
                return NULL;
            char *out = malloc(nitems + 1);
            if (!out)
            {
                XFree(data);
                return NULL;
            }
            memcpy(out, data, nitems);
            out[nitems] = '\0';
            XFree(data);
            XDeleteProperty(d, win, prop);
            return out;
        }
        if (time(NULL) - start > 1)
            return NULL;
        usleep(5000);
    }
}

/* ---------- helper: draw a single UTF-8 string using fontset (multibyte) ---------- */
static void draw_utf8(const char *s, int x, int y)
{
    if (!s)
        return;
    if (!fontset || !dpy)
    {
        XDrawString(dpy, win, gc, x, y, s, (int)strlen(s));
    }
    else
    {
        XmbDrawString(dpy, win, fontset, gc, x, y, s, (int)strlen(s));
    }
}

/* ---------- helper: width of a UTF-8 string in pixels ---------- */
static int utf8_width(const char *s)
{
    if (!s)
        return 0;
    if (!fontset || !dpy)
    {
        return XTextWidth(fontinfo, s, (int)strlen(s));
    }
    else
    {
        return XmbTextEscapement(fontset, s, (int)strlen(s));
    }
}

/* compute pixel width of first `bytes` bytes of utf8 string `s` */
static int utf8_prefix_width(const char *s, size_t bytes)
{
    if (!s || bytes == 0)
        return 0;
    char tmp[8192];
    size_t cp = bytes < sizeof(tmp) - 1 ? bytes : (sizeof(tmp) - 1);
    memcpy(tmp, s, cp);
    tmp[cp] = '\0';
    return utf8_width(tmp);
}

/* ---------- redraw main window (output first, prompt after output) ---------- */
static void redraw(void)
{
    if (!dpy)
        return;
    XClearWindow(dpy, win);

    /* tab bar at very top */
    int tab_h = line_height + 6;
    int x = 4;
    int tcount = tabs_count();
    for (int i = 0; i < tcount; ++i)
    {
        char lab[32];
        snprintf(lab, sizeof(lab), " Tab %d ", i + 1);
        int w = utf8_width(lab) + 8;

        if (i == active)
        {
            /* Highlight active tab with filled rectangle */
            XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
            XFillRectangle(dpy, win, gc, x - 2, 4, w, tab_h);
            /* Draw text in black on top of white */
            XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
            draw_utf8(lab, x + 4, 4 + fontinfo->ascent + 2);
        }
        else
        {
            /* Normal tab (non-active) */
            XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
            XDrawRectangle(dpy, win, gc, x - 2, 4, w, tab_h);
            draw_utf8(lab, x + 4, 4 + fontinfo->ascent + 2);
        }

        x += w + 6;
    }

    /* layout: output area begins below tab bar, prompt+input appears after output */
    int top = tab_h + 12;
    int bottom = win_h - 12;
    int avail_h = bottom - top;
    if (avail_h < line_height)
        avail_h = line_height;
    int max_lines = avail_h / line_height;
    if (max_lines < 1)
        max_lines = 1;

    /* If we have an active tab, render its output and prompt+input. Otherwise show prompt at top. */
    if (active >= 0 && active < tcount)
    {
        Tab *t = tabs_get(active);

        /* read any available child output */
        tabs_read_once(active);

        /* --- split output buffer into lines (outcopy + array of pointers) --- */
        int total_lines = 0;
        char *outcopy = NULL;
        char **out_lines = NULL;
        if (t->out_len > 0 && t->out_buf)
        {
            outcopy = malloc(t->out_len + 1);
            if (outcopy)
            {
                memcpy(outcopy, t->out_buf, t->out_len);
                outcopy[t->out_len] = '\0';
                /* worst case number of lines <= out_len+1, but allocate modestly for memory */
                /* We'll first count lines */
                size_t cap = 64;
                out_lines = malloc(sizeof(char *) * cap);
                if (out_lines)
                {
                    char *p = outcopy;
                    out_lines[total_lines++] = p;
                    for (size_t i = 0; i < (size_t)t->out_len; ++i)
                    {
                        if (outcopy[i] == '\n')
                        {
                            outcopy[i] = '\0';
                            if (i + 1 < (size_t)t->out_len)
                            {
                                if (total_lines >= (int)cap)
                                {
                                    cap *= 2;
                                    char **tmp = realloc(out_lines, sizeof(char *) * cap);
                                    if (!tmp)
                                        break;
                                    out_lines = tmp;
                                }
                                out_lines[total_lines++] = outcopy + i + 1;
                            }
                        }
                    }
                }
                else
                {
                    free(outcopy);
                    outcopy = NULL;
                }
            }
        }

        /* decide how many output lines to display, reserve rows for prompt+at least one input line */
        int reserve_for_input = 1;
        int can_show = max_lines - reserve_for_input;
        if (can_show < 0)
            can_show = 0;
        int show_lines = 0;
        if (out_lines && total_lines > 0)
        {
            show_lines = (total_lines <= can_show) ? total_lines : can_show;
        }

        /* Starting y for first displayed output line (top-down) */
        int y = top + line_height;

        /* If there are more total_lines than show_lines, display last show_lines */
        int start_idx = (total_lines > show_lines) ? (total_lines - show_lines) : 0;
        XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
        for (int i = start_idx; out_lines && i < total_lines; ++i)
        {
            draw_utf8(out_lines[i], 6, y);
            y += line_height;
        }

        /* --- Now draw the prompt at y (immediately after output) --- */
        draw_utf8(PROMPT, 6, y);

        /* --- prepare input buffer and cursor --- */
        const char *buf = "";
        size_t buflen = 0;
        size_t cursor = 0; /* byte offset from buffer start */
        if (t->editor)
        {
            buf = le_get_buffer(t->editor);
            buflen = le_get_length(t->editor);
            cursor = le_get_cursor(t->editor);
        }
        else
        {
            buf = t->input;
            buflen = (size_t)t->input_len;
            cursor = (size_t)t->input_pos;
        }
        if (!buf)
        {
            buf = "";
            buflen = 0;
            cursor = 0;
        }

        /* Split input buffer into lines (inplace using a copy) */
        char *bufcopy = NULL;
        char **in_lines = NULL;
        int in_lines_count = 0;
        if (buflen > 0)
        {
            bufcopy = malloc(buflen + 1);
            if (bufcopy)
            {
                memcpy(bufcopy, buf, buflen);
                bufcopy[buflen] = '\0';
                /* allocate small array for input lines */
                int cap = 8;
                in_lines = malloc(sizeof(char *) * cap);
                if (in_lines)
                {
                    char *p = bufcopy;
                    in_lines[in_lines_count++] = p;
                    for (size_t i = 0; i < buflen; ++i)
                    {
                        if (bufcopy[i] == '\n')
                        {
                            bufcopy[i] = '\0';
                            if (i + 1 <= buflen)
                            {
                                if (in_lines_count >= cap)
                                {
                                    cap *= 2;
                                    char **tmp = realloc(in_lines, sizeof(char *) * cap);
                                    if (!tmp)
                                        break;
                                    in_lines = tmp;
                                }
                                in_lines[in_lines_count++] = bufcopy + i + 1;
                            }
                        }
                    }
                }
                else
                {
                    free(bufcopy);
                    bufcopy = NULL;
                }
            }
        }

        /* Draw first input line on same baseline as prompt */
        int prompt_w = utf8_width(PROMPT);
        int input_x = 6 + prompt_w;
        if (in_lines_count > 0)
        {
            draw_utf8(in_lines[0], input_x, y);
        }
        else if (buflen == 0)
        {
            /* nothing to draw */
        }
        else
        {
            /* buffer present but splitting failed; draw whole buffer */
            draw_utf8(buf, input_x, y);
        }

        /* Draw subsequent input lines below */
        int in_y = y + line_height;
        for (int i = 1; in_lines && i < in_lines_count; ++i)
        {
            draw_utf8(in_lines[i], 6 + utf8_width(PROMPT), in_y);
            in_y += line_height;
        }

        /* --- compute cursor position in rendered coordinates (robust, from raw bytes) --- */
        int cur_line = 0;
        size_t byte_into_line = 0;

        if (buflen == 0)
        {
            cur_line = 0;
            byte_into_line = 0;
        }
        else if (in_lines && in_lines_count > 0 && bufcopy)
        {
            /* Walk line-by-line using the splitted lines to find which line contains the cursor */
            size_t seen = 0;
            int found = 0;
            for (int i = 0; i < in_lines_count; ++i)
            {
                size_t llen = strlen(in_lines[i]);
                if (cursor <= seen + llen)
                {
                    cur_line = i;
                    byte_into_line = cursor - seen;
                    found = 1;
                    break;
                }
                seen += llen;
                /* account for the removed newline in copy */
                if (i < in_lines_count - 1)
                    seen += 1;
            }
            if (!found)
            {
                /* cursor may be at end */
                if (cursor == buflen)
                {
                    if (buflen > 0 && buf[buflen - 1] == '\n')
                    {
                        /* new empty line after trailing newline */
                        cur_line = in_lines_count;
                        byte_into_line = 0;
                    }
                    else
                    {
                        /* last character of last line */
                        cur_line = in_lines_count - 1;
                        size_t sum = 0;
                        for (int i = 0; i < cur_line; ++i)
                        {
                            sum += strlen(in_lines[i]);
                            sum += 1;
                        }
                        byte_into_line = cursor - sum;
                    }
                }
                else
                {
                    /* fallback: put cursor on last line */
                    cur_line = in_lines_count - 1;
                    size_t sum = 0;
                    for (int i = 0; i < cur_line; ++i)
                    {
                        sum += strlen(in_lines[i]);
                        sum += 1;
                    }
                    if (cursor >= sum)
                        byte_into_line = cursor - sum;
                    else
                        byte_into_line = 0;
                }
            }
        }
        else
        {
            /* no split available — fall back to naive scan for newlines in original buffer */
            size_t last_nl = 0;
            int lines_seen = 0;
            for (size_t i = 0; i < cursor; ++i)
            {
                if (buf[i] == '\n')
                {
                    lines_seen++;
                    last_nl = i + 1;
                }
            }
            cur_line = lines_seen;
            byte_into_line = cursor - last_nl;
        }

        /* Map cur_line to screen coords */
        int cursor_screen_y;
        if (cur_line <= 0)
            cursor_screen_y = y;
        else
            cursor_screen_y = y + cur_line * line_height;

        /* compute x offset in pixels for the cursor by measuring bytes up to cursor in that line */
        int px = 0;
        if (cur_line < in_lines_count && in_lines && bufcopy)
        {
            const char *line_ptr = in_lines[cur_line];
            size_t line_len = strlen(line_ptr);
            size_t safe_bytes = byte_into_line <= line_len ? byte_into_line : line_len;
            px = utf8_prefix_width(line_ptr, safe_bytes);
        }
        else if (cur_line == in_lines_count && (buflen > 0 && buf[buflen - 1] == '\n'))
        {
            /* cursor on new empty line after a trailing newline */
            px = 0;
        }
        else
        {
            /* fallback: measure entire buffer prefix up to cursor */
            px = utf8_prefix_width(buf, cursor);
        }

        /* final cursor x: use a single base_x for the line so text drawing and cursor position align */
        int base_x = (cur_line == 0) ? input_x : (6 + utf8_width(PROMPT));
        int cursor_screen_x = base_x + px;

        /* small visual nudge: X font drawing and the pixel measurement sometimes differ by 1px;
           subtract 1 px when px > 0 so the cursor lines up visually (guard to not go negative). */
        if (px > 0 && cursor_screen_x > 0)
        {
            cursor_screen_x = cursor_screen_x - 1;
        }

        /* draw cursor */
        int cursor_top = cursor_screen_y - fontinfo->ascent;
        int cursor_h = line_height;
        XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
        XFillRectangle(dpy, win, gc, cursor_screen_x, cursor_top, 2, cursor_h);

        /* cleanup temporaries */
        if (out_lines)
            free(out_lines);
        if (outcopy)
            free(outcopy);
        if (in_lines)
            free(in_lines);
        if (bufcopy)
            free(bufcopy);
    }
    else
    {
        /* no active tab - draw prompt at top area */
        int y = top + line_height;
        draw_utf8(PROMPT, 6, y);
    }

    XFlush(dpy);
}

/* --- GUI History Search Prompt (Ctrl+R) --- */
static void gui_history_search_prompt(Display *d, Window win, int tab_idx)
{
    if (!d)
        return;

    LineEditor *le = le_create("Enter search term: ");
    if (!le)
        return;
    le_set_term_mode(le, 0); /* disable stdout redraws for GUI */

    int prompt_y = (line_height + 6) + 12 + fontinfo->ascent;
    int done = 0;

    /* --- Immediate draw: show search bar right away --- */
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
    XFillRectangle(dpy, win, gc, 40, prompt_y - line_height, win_w - 80, line_height + 8);
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    draw_utf8("Enter search term: ", 46, prompt_y);
    draw_utf8(le_get_buffer(le), 46 + utf8_width("Enter search term: "), prompt_y);
    XFlush(dpy);

    while (!done)
    {
        XEvent ev;
        XNextEvent(d, &ev);

        if (ev.type == KeyPress)
        {
            KeySym ks = NoSymbol;
            char buf[1024];
            int len = 0;

            if (xic)
            {
                Status status = 0;
                len = XmbLookupString(xic, &ev.xkey, buf, (int)sizeof(buf) - 1, &ks, &status);
                if (len < 0)
                    len = 0;
                buf[len] = '\0';
            }
            else
            {
                len = XLookupString(&ev.xkey, buf, (int)sizeof(buf) - 1, &ks, NULL);
                if (len < 0)
                    len = 0;
                buf[len] = '\0';
            }

            /* Handle Enter/Return */
            if ((ks == XK_Return || ks == XK_KP_Enter) ||
                (len == 1 && ((unsigned char)buf[0] == '\r' || (unsigned char)buf[0] == '\n')))
            {
                done = 1;
                break;
            }

            /* Handle Ctrl-C or ESC cancel */
            if (len == 1)
            {
                unsigned char c = (unsigned char)buf[0];
                if (c == 0x03 || c == 0x1B)
                {
                    done = 1;
                    break;
                }
            }

            /* Ignore Tab */
            if (ks == XK_Tab)
                continue;

            /* --- Handle Backspace/Delete properly --- */
            if (ks == XK_BackSpace || ks == XK_Delete ||
                (len == 1 && ((unsigned char)buf[0] == 0x7F)))
            {
                le_backspace(le); /* delete last character */
            }
            else if (len > 0)
            {
                le_feed_bytes(le, buf, (size_t)len);
            }

            /* --- Redraw prompt area after every change --- */
            XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
            XFillRectangle(dpy, win, gc, 40, prompt_y - line_height, win_w - 80, line_height + 8);
            XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
            draw_utf8("Enter search term: ", 46, prompt_y);
            draw_utf8(le_get_buffer(le), 46 + utf8_width("Enter search term: "), prompt_y);
            XFlush(dpy);
        }
    }

    const char *term = le_get_buffer(le);
    if (term && term[0] != '\0')
    {
        history_search_and_output(tab_idx, term);
    }
    else
    {
        const char *msg = "History search cancelled or empty\n";
        tabs_append_output(tab_idx, msg, (ssize_t)strlen(msg));
    }

    le_destroy(le);
    redraw();
}

/* Clear and free autocomplete state for a tab (safe to call multiple times) */
// static void clear_comp_state(Tab *t)
// {
//     if (!t)
//         return;
//     if (t->comp_matches)
//     {
//         for (int i = 0; i < t->comp_count; ++i)
//         {
//             free(t->comp_matches[i]);
//         }
//         free(t->comp_matches);
//         t->comp_matches = NULL;
//     }
//     t->comp_count = 0;
//     t->comp_pending = 0;
//     t->comp_token_start = 0;
//     t->comp_token_len = 0;
//     if (t->comp_dir)
//     {
//         free(t->comp_dir);
//         t->comp_dir = NULL;
//     }
// }

/* ---------- main ---------- */
int main(void)
{
    /* set locale for multibyte (UTF-8) handling */
    setlocale(LC_ALL, "");
    setlocale(LC_CTYPE, "");
    setup_signal_handlers(); // ✅ Install safe SIGINT handler

    /* Prefer user X modifiers (XMODIFIERS) for X input methods (IM).
       If XMODIFIERS is not present, fall back to empty string. */
    const char *xm = getenv("XMODIFIERS");
    if (!xm)
        xm = "";
    XSetLocaleModifiers(xm);

    /* initialize history (~/.myterm_history) */
    if (history_init(NULL) != 0)
    {
        fprintf(stderr, "Warning: could not load history file\n");
    }
    atexit(history_save);

    tabs_init();

    /* create notify pipe BEFORE opening X so app can signal main loop */
    notify_pipe_read = notify_pipe_write = -1;
    {
        int p[2];
        if (pipe(p) == 0)
        {
            notify_pipe_read = p[0];
            notify_pipe_write = p[1];
            /* make read end non-blocking for safe draining in select() */
            int flags = fcntl(notify_pipe_read, F_GETFL, 0);
            if (flags >= 0)
                fcntl(notify_pipe_read, F_SETFL, flags | O_NONBLOCK);
            tabs_set_notify_fd(notify_pipe_write);
        }
        else
        {
            notify_pipe_read = notify_pipe_write = -1;
            tabs_set_notify_fd(-1);
        }
    }

    /* create X window and set black background + GC default foreground = white */
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        die("XOpenDisplay");
    int scr = DefaultScreen(dpy);

    /* create window with black background */
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
                              10, 10, win_w, win_h, 1,
                              BlackPixel(dpy, scr), /* border */
                              BlackPixel(dpy, scr) /* background = black */);

    XStoreName(dpy, win, "MyTerm");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask | SelectionClear);

    /* ensure background is black for XClearWindow */
    XSetWindowBackground(dpy, win, BlackPixel(dpy, scr));

    XMapWindow(dpy, win);

    /* create GC and set default drawing color to white */
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, WhitePixel(dpy, scr));

    xim = XOpenIM(dpy, NULL, NULL, NULL);
    if (xim)
    {
        /* prefer simple preedit/status (we don't use preedit in GUI) */
        xic = XCreateIC(xim,
                        XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                        XNClientWindow, win,
                        NULL);
        /* it's okay if xic is NULL; we will fallback to XLookupString */
    }

    fontinfo = XLoadQueryFont(dpy, "-misc-fixed-*-*-*-*-14-*-*-*-*-*-*-*");
    if (!fontinfo)
        fontinfo = XLoadQueryFont(dpy, "fixed");
    if (!fontinfo)
        die("XLoadQueryFont");
    XSetFont(dpy, gc, fontinfo->fid);

    char *base_fonts = "Noto Sans, Noto Sans Devanagari, DejaVu Sans, Arial Unicode MS, -misc-fixed-*-*-*-*-14-*-*-*-*-*-*-*";
    char **missing;
    int missing_count;
    char *def;
    fontset = XCreateFontSet(dpy, base_fonts, &missing, &missing_count, &def);
    if (!fontset)
    {
        fontset = XCreateFontSet(dpy, "fixed", &missing, &missing_count, &def);
    }

    line_height = fontinfo->ascent + fontinfo->descent + 2;

    int id = tabs_create();
    if (id < 0)
        die("tabs_create");
    active = id;
    /* ensure prompt is set in editor if present, and disable terminal-mode redraw (GUI mode) */
    Tab *t0 = tabs_get(active);
    if (t0 && t0->editor)
    {
        le_set_prompt(t0->editor, PROMPT);
        le_set_term_mode(t0->editor, 0); /* 0 = GUI mode: do not write to stdout */
    }

    redraw();

    while (1)
    {
        /* process any pending X events first */
        while (XPending(dpy))
        {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose)
                redraw();
            else if (ev.type == ConfigureNotify)
            {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
                redraw();
            }
            else if (ev.type == KeyPress)
            {
                KeySym ks = NoSymbol;
                char buf[1024];
                int len = 0;

                /* Prefer input method (XIC) for multibyte / composed characters.
                If unavailable, fall back to XLookupString. */
                if (xic)
                {
                    Status status = 0;
                    /* XmbLookupString returns number of bytes written (UTF-8) */
                    len = XmbLookupString(xic, &ev.xkey, buf, (int)sizeof(buf) - 1, &ks, &status);
                    if (len < 0)
                        len = 0;
                    buf[len] = '\0';
                }
                else
                {
                    len = XLookupString(&ev.xkey, buf, (int)sizeof(buf) - 1, &ks, NULL);
                    if (len < 0)
                        len = 0;
                    buf[len] = '\0';
                }

                if (active >= 0)
                {
                    Tab *t = tabs_get(active);
                    if (t && t->comp_pending)
                    {
                        /* If user pressed ESC (cancel) while pending, clear state */
                        if (len == 1 && (unsigned char)buf[0] == 0x1B)
                        {
                            /* free stored matches */
                            for (int i = 0; i < t->comp_count; ++i)
                            {
                                free(t->comp_matches[i]);
                            }
                            free(t->comp_matches);
                            t->comp_matches = NULL;
                            t->comp_count = 0;
                            t->comp_pending = 0;
                            if (t->comp_dir)
                            {
                                free(t->comp_dir);
                                t->comp_dir = NULL;
                            }
                            need_redraw = 1;
                            /* consume key */
                            continue;
                        }

                        /* single digit keys 1..9 choose an item */
                        if (len == 1)
                        {
                            unsigned char ch = (unsigned char)buf[0];
                            if (ch >= '1' && ch <= '9')
                            {
                                int idx = (int)(ch - '1');
                                if (idx < t->comp_count)
                                {
                                    /* build full insertion (prefix dir + filename) */
                                    const char *name = t->comp_matches[idx];
                                    char ins[INPUT_MAX];
                                    /* Only prepend comp_dir if it is non-NULL and not the special "./" marker.
                                    This prevents inserting "./" when the user didn't type any directory. */
                                    if (t->comp_dir && strcmp(t->comp_dir, "./") != 0)
                                    {
                                        snprintf(ins, sizeof(ins), "%s%s", t->comp_dir, name);
                                    }
                                    else
                                    {
                                        snprintf(ins, sizeof(ins), "%s", name);
                                    }

                                    /* Insert/replace at saved token start */
                                    int start = t->comp_token_start;
                                    int orig_len = t->comp_token_len;
                                    int tail_start = start + orig_len;
                                    int tail_len = t->input_len - tail_start;
                                    int ins_len = (int)strlen(ins);
                                    if (start + ins_len + tail_len >= INPUT_MAX - 1)
                                    {
                                        ins_len = (INPUT_MAX - 1) - start - tail_len;
                                    }

                                    if (t->editor)
                                    {
                                        /* use editor helper (make sure le_replace_last_word exists) */
                                        le_replace_last_word(t->editor, ins);
                                    }
                                    else
                                    {
                                        /* replace in t->input */
                                        memmove(t->input + start + ins_len, t->input + tail_start, tail_len + 1);
                                        memcpy(t->input + start, ins, (size_t)ins_len);
                                        t->input_len = start + ins_len + tail_len;
                                        t->input[t->input_len] = '\0';
                                        t->input_pos = start + ins_len;
                                    }

                                    /* clear comp state and free memory */
                                    for (int j = 0; j < t->comp_count; ++j)
                                        free(t->comp_matches[j]);
                                    free(t->comp_matches);
                                    t->comp_matches = NULL;
                                    t->comp_count = 0;
                                    t->comp_pending = 0;
                                    if (t->comp_dir)
                                    {
                                        free(t->comp_dir);
                                        t->comp_dir = NULL;
                                    }

                                    need_redraw = 1;
                                    /* consumed the digit; do not let it fall through */
                                    continue;
                                }
                                else
                                {
                                    /* invalid index -> print message and ignore */
                                    const char *msg = "Invalid choice\n";
                                    tabs_append_output(active, msg, (ssize_t)strlen(msg));
                                    need_redraw = 1;
                                    continue;
                                }
                            }
                        }

                        /* If comp_pending is set and we get other keys (like printable chars),
                           you may want to cancel pending state — optional. We'll leave pending
                           until user explicitly picks a number or presses ESC. */
                    }
                }

                /* Early: handle single-byte control codes (Ctrl-A / Ctrl-E / Ctrl-C / Ctrl-Z / Ctrl-R) */
                if (len == 1)
                {
                    unsigned char c = (unsigned char)buf[0];
                    /* Ctrl-C (ETX) */
                    if (c == 0x03)
                    {
                        if (active >= 0)
                        {
                            multiwatch_interrupt(active);
                            cmd_exec_interrupt_tab(active);
                            need_redraw = 1;
                        }
                        continue;
                    }
                    /* Ctrl-Z (suspend) */
                    /* Ctrl-Z (suspend) */
                    if (c == 0x1A)
                    {
                        if (active >= 0)
                        {
                            /* First notify multiwatch if running (no-op if not). */
                            multiwatch_interrupt(active);

                            /* Ask cmd_exec to suspend (sends SIGTSTP to the process group). */
                            int rc = cmd_exec_suspend_tab(active);

                            /* Provide user-visible confirmation in the terminal output */
                            if (rc == 0)
                            {
                                char msg[128];
                                /* Using tab index is simpler than trying to obtain a PID here.
                                   If you later add a getter to cmd_exec to return PGID/PID,
                                   you can include the PID in this message. */
                                int L = snprintf(msg, sizeof(msg), "\n[process in tab %d stopped by Ctrl+Z]\n", active + 1);
                                tabs_append_output(active, msg, (ssize_t)L);
                            }
                            else
                            {
                                const char *err = "\n[no foreground process to stop]\n";
                                tabs_append_output(active, err, (ssize_t)strlen(err));
                            }

                            need_redraw = 1;
                        }
                        continue;
                    }

                    /* Ctrl-A: move to start */
                    if (c == 0x01)
                    {
                        if (active >= 0)
                        {
                            Tab *t = tabs_get(active);
                            if (t)
                            {
                                if (t->editor)
                                    le_feed_byte(t->editor, 0x01);
                                else
                                    t->input_pos = 0;
                                need_redraw = 1;
                            }
                        }
                        continue;
                    }
                    /* Ctrl-E: move to end */
                    if (c == 0x05)
                    {
                        if (active >= 0)
                        {
                            Tab *t = tabs_get(active);
                            if (t)
                            {
                                if (t->editor)
                                    le_feed_byte(t->editor, 0x05);
                                else
                                    t->input_pos = t->input_len;
                                need_redraw = 1;
                            }
                        }
                        continue;
                    }
                    /* Ctrl-R: trigger history search prompt */
                    if (c == 0x12)
                    { /* Ctrl+R */
                        if (active >= 0)
                        {
                            gui_history_search_prompt(dpy, win, active);
                            need_redraw = 1;
                        }
                        continue;
                    }
                }

                int ctrl = (ev.xkey.state & ControlMask) != 0;
                int shift = (ev.xkey.state & ShiftMask) != 0;

                /* Paste: Ctrl+V or Shift+Insert */
                if ((ctrl && (ks == XK_v || ks == XK_V)) || (ks == XK_Insert && shift))
                {
                    char *clip = get_clipboard_text(dpy);
                    if (clip && active >= 0)
                    {
                        Tab *t = tabs_get(active);
                        if (t->editor)
                        {
                            size_t clen = strlen(clip);
                            size_t avail = LE_MAX_INPUT - 1 - le_get_length(t->editor);
                            if (clen > avail)
                                clen = avail;
                            le_feed_bytes(t->editor, clip, clen);
                        }
                        else
                        {
                            size_t clen = strlen(clip);
                            if (t->input_len + (int)clen < INPUT_MAX - 1)
                            {
                                /* append clip */
                                memcpy(t->input + t->input_len, clip, clen);
                                t->input_len += (int)clen;
                                t->input[t->input_len] = '\0';
                                t->input_pos = t->input_len;
                            }
                        }
                        free(clip);
                        need_redraw = 1;
                    }
                    else if (clip)
                        free(clip);
                    continue;
                }

                /* F-keys and regular handling (unchanged) */
                if (ks == XK_F1)
                {
                    int nid = tabs_create();
                    if (nid >= 0)
                    {
                        active = nid;
                        Tab *nt = tabs_get(active);
                        if (nt && nt->editor)
                        {
                            le_set_prompt(nt->editor, PROMPT);
                            le_set_term_mode(nt->editor, 0); /* disable terminal-mode redraw for GUI */
                        }
                    }
                    need_redraw = 1;
                }
                else if (ks == XK_F2)
                {
                    int c = tabs_count();
                    if (c > 0)
                    {
                        active = (active + 1) % c;
                        Tab *nt = tabs_get(active);
                        if (nt && nt->editor)
                        {
                            le_set_prompt(nt->editor, PROMPT);
                            le_set_term_mode(nt->editor, 0);
                        }
                    }
                    need_redraw = 1;
                }
                else if (ks == XK_F3)
                {
                    if (active >= 0)
                    {
                        tabs_close(active);
                        if (tabs_count() > 0)
                            active = (active - 1 < 0) ? 0 : active - 1;
                        else
                            active = -1;
                    }
                    need_redraw = 1;
                }
                else if (ks == XK_BackSpace)
                {
                    if (active >= 0)
                    {
                        Tab *t = tabs_get(active);
                        if (t->editor)
                            le_feed_byte(t->editor, 0x7f);
                        else
                        {
                            if (t->input_len > 0)
                            {
                                int i = t->input_len - 1;
                                while (i >= 0 && (t->input[i] & 0xC0) == 0x80)
                                    --i;
                                if (i < 0)
                                {
                                    t->input_len = 0;
                                    t->input[0] = '\0';
                                }
                                else
                                {
                                    t->input_len = i;
                                    t->input[t->input_len] = '\0';
                                }
                                t->input_pos = t->input_len;
                            }
                        }
                        need_redraw = 1;
                    }
                }
                /* ENTER / RETURN handling: treat as Enter if either KeySym indicates Return
   OR the produced text bytes contain a CR/LF. This handles cases where
   XmbLookupString/XIC doesn't set ks but returns '\n' in buf. */
                else if ((ks == XK_Return || ks == XK_KP_Enter) ||
                         (len == 1 && ((unsigned char)buf[0] == '\r' || (unsigned char)buf[0] == '\n')))
                {
                    if (active >= 0)
                    {
                        Tab *t = tabs_get(active);
                        const char *bufptr = NULL;
                        size_t blen = 0;
                        if (t->editor)
                        {
                            bufptr = le_get_buffer(t->editor);
                            blen = le_get_length(t->editor);
                        }
                        else
                        {
                            bufptr = t->input;
                            blen = t->input_len;
                        }

                        /* If buffer is empty, do nothing */
                        if (blen > 0)
                        {
                            /* If there's an unclosed quote, insert a newline into the editor/input
                               rather than submitting the command */
                            if (input_has_unclosed_quote_buf(bufptr, blen))
                            {
                                if (t->editor)
                                    le_feed_bytes(t->editor, "\n", 1); /* insert newline (use multibyte API) */
                                else
                                {
                                    if (t->input_len + 1 < INPUT_MAX - 1)
                                    {
                                        t->input[t->input_len++] = '\n';
                                        t->input[t->input_len] = '\0';
                                    }
                                }
                                need_redraw = 1;
                            }
                            else
                            {
                                /* No unclosed quotes -> submit the line */
                                char cmdline[INPUT_MAX];
                                size_t copylen = blen < sizeof(cmdline) - 1 ? blen : sizeof(cmdline) - 1;
                                memcpy(cmdline, bufptr, copylen);
                                cmdline[copylen] = '\0';

                                char echo_line[INPUT_MAX + 64];
                                int L = snprintf(echo_line, sizeof(echo_line), "%s%s\n", PROMPT, cmdline);
                                tabs_append_output(active, echo_line, L);
                                need_redraw = 1;

                                cmd_exec_run_in_tab(active, cmdline);

                                if (t->editor)
                                    le_reset(t->editor);
                                t->input_len = 0;
                                t->input[0] = '\0';
                                t->input_pos = 0;
                                need_redraw = 1;
                            }
                        }
                    }
                }

                else if (ks == XK_Tab)
                {
                    if (active >= 0)
                    {
                        /* try to autocomplete for current active tab */
                        (void)autocomplete_try(active);
                        need_redraw = 1;
                    }
                    continue;
                }
                else
                {
                    /* Default: printable / multibyte characters */
                    if (len > 0 && active >= 0)
                    {
                        Tab *t = tabs_get(active);
                        if (t->editor)
                        {
                            le_feed_bytes(t->editor, buf, (size_t)len);
                        }
                        else
                        {
                            if (t->input_len + len < INPUT_MAX - 1)
                            {
                                memcpy(t->input + t->input_len, buf, len);
                                t->input_len += len;
                                t->input[t->input_len] = '\0';
                                t->input_pos = t->input_len;
                            }
                        }
                        need_redraw = 1;
                    }
                }
            }
        }

        // ✅ Handle Ctrl+C interrupt safely (main thread context)
        if (interrupt_flag)
        {
            if (active >= 0)
            {
                multiwatch_interrupt(active);
                cmd_exec_interrupt_tab(active);
                need_redraw = 1;
            }
            interrupt_flag = 0; // Reset flag
        }

        if (need_redraw)
        {
            redraw();
            need_redraw = 0;
        }

        /* select on persistent shell fds (from_child_fd) + notify pipe */
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < tabs_count(); ++i)
        {
            int fd = tabs_get_fd(i);
            if (fd >= 0)
            {
                FD_SET(fd, &rfds);
                if (fd > maxfd)
                    maxfd = fd;
            }
        }
        if (notify_pipe_read >= 0)
        {
            FD_SET(notify_pipe_read, &rfds);
            if (notify_pipe_read > maxfd)
                maxfd = notify_pipe_read;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20000; /* 20ms */
        if (maxfd >= 0)
        {
            int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
            if (ready > 0)
            {
                /* drain notify pipe first (if any) */
                if (notify_pipe_read >= 0 && FD_ISSET(notify_pipe_read, &rfds))
                {
                    char drain[256];
                    /* read until empty */
                    while (read(notify_pipe_read, drain, sizeof(drain)) > 0)
                    { /* discard */
                    }
                    /* output was already appended by writer — request redraw */
                    need_redraw = 1;
                }

                for (int i = 0; i < tabs_count(); ++i)
                {
                    int fd = tabs_get_fd(i);
                    if (fd >= 0 && FD_ISSET(fd, &rfds))
                    {
                        tabs_read_once(i);
                        if (i == active)
                            need_redraw = 1;
                    }
                }
            }
        }
        else
        {
            usleep(100000);
        }
    }

    /* unreachable normally, but tidy up if we ever get here */
    tabs_cleanup();

    if (notify_pipe_read >= 0)
        close(notify_pipe_read);
    if (notify_pipe_write >= 0)
    {
        tabs_set_notify_fd(-1);
        close(notify_pipe_write);
    }

    if (dpy)
    {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
    }

    history_save();
    return 0;
}
