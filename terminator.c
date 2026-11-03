#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#define MAX_LINE     1024
#define MAX_TABS     8
#define MAX_ARGS     64
#define MAX_HISTORY  1000
#define HIST_SHOW    1000
#define SCR_BUF      32768
#define VISIBLE      26
#define MAX_JOBS     32

/* ---------- Tab ---------- */
typedef struct {
    int   active;
    char  screen[SCR_BUF];
    char  input[MAX_LINE];
    int   inlen;
    int   cursor;
    char  cwd[256];
    int   scroll;
    int   status;
    int   multiline;
    char  multi[MAX_LINE*2];
    pid_t fg_pid;
    int   search_mode;
    char  search[MAX_LINE];
    int   search_len;
} Tab;

Tab     tabs[MAX_TABS];
int     cur_tab = 0;
int     running = 1;
int     use_process = 0;

Display *dpy;
Window   win;
GC       gc;
int      scr;

char    hist[MAX_HISTORY][MAX_LINE];
int     histn = 0;

/* ---------- Job control ---------- */
typedef struct {
    int   id;
    pid_t pid;
    int   state;   /* 0 running, 1 stopped */
    char  cmd[MAX_LINE];
} Job;

Job   jobs[MAX_JOBS];
int   njobs = 0;

/* ---------- multiWatch ---------- */
typedef struct {
    int   active;
    int   n;
    pid_t pid[MAX_ARGS];
    int   fd[MAX_ARGS];
    char  cmd[MAX_ARGS][MAX_LINE];
    time_t last[MAX_ARGS];
} MW;

MW mw;

/* =====================================================
 *  History
 * ===================================================== */
history_add(char *line)
{
    if (!line || !*line) return;
    if (histn >= MAX_HISTORY) {
        int i;
        for (i = 0; i < MAX_HISTORY-1; i++)
            strcpy(hist[i], hist[i+1]);
        histn = MAX_HISTORY-1;
    }
    strncpy(hist[histn], line, MAX_LINE-1);
    hist[histn][MAX_LINE-1] = 0;
    histn++;
}

history_get(int idx, char *out)
{
    if (idx >= 0 && idx < histn) strcpy(out, hist[idx]);
    else out[0] = 0;
}

/* =====================================================
 *  Buffer helpers
 * ===================================================== */
buf_add(Tab *t, char *s)
{
    int used, space, n;
    used  = strlen(t->screen);
    space = SCR_BUF - used - 1;
    n     = strlen(s);
    if (n > space) {
        char *nl = strchr(t->screen, '\n');
        if (nl) {
            int drop = nl - t->screen + 1;
            memmove(t->screen, t->screen + drop, used - drop + 1);
            used -= drop;
            space = SCR_BUF - used - 1;
        }
    }
    if (n > space) n = space;
    if (n <= 0) return;
    memcpy(t->screen + used, s, n);
    t->screen[used + n] = 0;
}

buf_addf(Tab *t, char *fmt, char *a)
{
    char line[1400];
    snprintf(line, sizeof line, fmt, a);
    buf_add(t, line);
}

prompt(Tab *t)
{
    t->inlen = 0;
    t->cursor = 0;
    t->input[0] = 0;
}

/* =====================================================
 *  Redraw
 * ===================================================== */
redraw()
{
    Tab  *t = &tabs[cur_tab];
    char  copy[SCR_BUF];
    char *lines[2000];
    int   nlines = 0, i, start, end, y;
    char  cur[MAX_LINE + 64];
    char  header[128];

    XClearWindow(dpy, win);

    header[0] = 0;
    for (i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].active) continue;
        if (i == cur_tab) strcat(header, "[*");
        else strcat(header, "[");
        {
            char tmp[8]; snprintf(tmp, 8, "%d] ", i+1); strcat(header, tmp);
        }
    }
    XDrawString(dpy, win, gc, 10, 15, header, strlen(header));

    strcpy(copy, t->screen);
    lines[nlines++] = copy;
    for (i = 0; copy[i]; i++) {
        if (copy[i] == '\n') {
            copy[i] = 0;
            if (copy[i+1]) lines[nlines++] = &copy[i+1];
        }
    }
    if (t->scroll > nlines - VISIBLE) t->scroll = nlines - VISIBLE;
    if (t->scroll < 0) t->scroll = 0;
    end   = nlines - t->scroll;
    start = end - VISIBLE;
    if (start < 0) start = 0;

    y = 40;
    for (i = start; i < end; i++) {
        XDrawString(dpy, win, gc, 10, y, lines[i], strlen(lines[i]));
        y += 18;
    }

    if (t->search_mode)
        snprintf(cur, sizeof cur, "Enter search term: %s", t->search);
    else if (t->multiline)
        snprintf(cur, sizeof cur, "> %s", t->input);
    else
        snprintf(cur, sizeof cur, "$ %s", t->input);
    XDrawString(dpy, win, gc, 10, y, cur, strlen(cur));

    XFlush(dpy);
}

/* =====================================================
 *  Parsing / expansion
 * ===================================================== */
split_args(char *line, char **argv)
{
    char *tok;
    int n = 0;
    tok = strtok(line, " \t");
    while (tok && n < MAX_ARGS-1) {
        argv[n++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[n] = NULL;
    return n;
}

needs_more(char *line)
{
    int sq = 0, dq = 0, bt = 0, par = 0;
    char *p;
    size_t n;
    for (p = line; *p; p++) {
        if (*p == '\\' && !sq) { p++; continue; }
        if (*p == '\'' && !dq) { sq = !sq; continue; }
        if (*p == '"'  && !sq) { dq = !dq; continue; }
        if (*p == '`'  && !sq) { bt = !bt; continue; }
        if (*p == '$' && p[1] == '(' && !sq) { par++; p++; continue; }
        if (*p == ')' && !sq && par) { par--; continue; }
    }
    if (sq || dq || bt || par) return 1;
    n = strlen(line);
    if (n && line[n-1] == '\\' && !sq) return 1;
    return 0;
}

expand(char *in, char *out, int cap, int last_status)
{
    char *p = in;
    int   o = 0;
    int   sq = 0, dq = 0;
    char  name[64], inner[512];
    int   nl, il;
    char  buf[256];
    FILE *fp;
    size_t n;
    char *v;

    while (*p && o < cap-1) {
        if (*p == '\'' && !dq) { sq = !sq; out[o++] = *p++; continue; }
        if (*p == '"'  && !sq) { dq = !dq; out[o++] = *p++; continue; }
        if (*p == '\\' && !sq) {
            char nx = p[1];
            if (nx == '\\' || nx == '$' || nx == '`' || nx == '"' ||
                nx == '\'' || nx == '\n' || nx == ' ') {
                /* escaped char: emit the char, drop the backslash */
                p++;
            if (o < cap-1) out[o++] = *p++;
                } else if (nx == 0) {
                    /* dangling backslash at end: keep it */
                    if (o < cap-1) out[o++] = *p++;
                } else {
                    /* literal backslash — keep it */
                    if (o < cap-1) out[o++] = *p++;
                }
                continue;
        }
        if (*p == '$' && !sq) {
            if (p[1] == '?') { o += snprintf(out+o, cap-o, "%d", last_status); p += 2; continue; }
            if (p[1] == '(') {
                int depth = 1;
                char *q = p+2;
                while (*q && depth) {
                    if (*q == '(') depth++;
                    else if (*q == ')') depth--;
                    if (depth) q++;
                }
                if (!*q) { out[o++] = *p++; continue; }
                il = q - (p+2);
                if (il >= (int)sizeof inner) il = sizeof inner - 1;
                memcpy(inner, p+2, il); inner[il] = 0;
                fp = popen(inner, "r");
                if (fp) {
                    while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
                        if (o + (int)n < cap-1) { memcpy(out+o, buf, n); o += n; }
                        pclose(fp);
                }
                while (o && out[o-1] == '\n') o--;
                p = q + 1; continue;
            }
            if (isalpha((unsigned char)p[1]) || p[1] == '_') {
                nl = 0;
                char *q = p+1;
                while ((isalnum((unsigned char)*q) || *q == '_') && nl < 63)
                    name[nl++] = *q++;
                name[nl] = 0;
                v = getenv(name);
                if (v) {
                    int vl = strlen(v);
                    if (o + vl < cap-1) { memcpy(out+o, v, vl); o += vl; }
                }
                p = q; continue;
            }
        }
        if (*p == '`' && !sq) {
            char *q = strchr(p+1, '`');
            if (q) {
                il = q - (p+1);
                if (il >= (int)sizeof inner) il = sizeof inner - 1;
                memcpy(inner, p+1, il); inner[il] = 0;
                fp = popen(inner, "r");
                if (fp) {
                    while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
                        if (o + (int)n < cap-1) { memcpy(out+o, buf, n); o += n; }
                        pclose(fp);
                }
                while (o && out[o-1] == '\n') o--;
                p = q + 1; continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* =====================================================
 *  Job control
 * ===================================================== */
job_add(pid_t pid, char *cmd)
{
    int i, cand, used;
    if (njobs >= MAX_JOBS) return;
    jobs[njobs].id = 0;
    for (cand = 1; cand <= MAX_JOBS; cand++) {
        used = 0;
        for (i = 0; i < njobs; i++) if (jobs[i].id == cand) { used = 1; break; }
        if (!used) { jobs[njobs].id = cand; break; }
    }
    jobs[njobs].pid = pid;
    jobs[njobs].state = 0;
    strncpy(jobs[njobs].cmd, cmd, MAX_LINE-1);
    jobs[njobs].cmd[MAX_LINE-1] = 0;
    njobs++;
}

job_reap(Tab *t)
{
    int i, w, st;
    pid_t r;
    char msg[1024];
    for (i = 0; i < njobs; i++) {
        r = waitpid(jobs[i].pid, &st, WNOHANG | WUNTRACED | WCONTINUED);
        if (r <= 0) continue;
        if (WIFEXITED(st) || WIFSIGNALED(st)) jobs[i].state = 2;
        else if (WIFSTOPPED(st)) {
            jobs[i].state = 1;
            snprintf(msg, sizeof msg, "\n[%d]+ Stopped %s\n", jobs[i].id, jobs[i].cmd);
            buf_add(t, msg);
        } else if (WIFCONTINUED(st)) jobs[i].state = 0;
    }
    w = 0;
    for (i = 0; i < njobs; i++) {
        if (jobs[i].state == 2) continue;
        if (w != i) jobs[w] = jobs[i];
        w++;
    }
    njobs = w;
}

/* =====================================================
 *  multiWatch
 * ===================================================== */
mw_start(char **cmds, int n)
{
    int i;
    mw.active = 1;
    mw.n = n;
    for (i = 0; i < n; i++) {
        strncpy(mw.cmd[i], cmds[i], MAX_LINE-1);
        mw.cmd[i][MAX_LINE-1] = 0;
        mw.pid[i] = 0;
        mw.fd[i]  = -1;
        mw.last[i] = 0;
    }
}

mw_tick(Tab *t)
{
    int i, fds[2], p;
    char line[1400];
    if (!mw.active) return;
    for (i = 0; i < mw.n; i++) {
        if (mw.pid[i] > 0) {
            int st;
            if (waitpid(mw.pid[i], &st, WNOHANG) == mw.pid[i]) {
                mw.pid[i] = 0;
                if (mw.fd[i] >= 0) { close(mw.fd[i]); mw.fd[i] = -1; }
            }
            continue;
        }
        if (time(NULL) - mw.last[i] < 2) continue;
        if (pipe(fds) < 0) continue;
        p = fork();
        if (p == 0) {
            close(fds[0]);
            dup2(fds[1], 1); dup2(fds[1], 2);
            close(fds[1]);
            execl("/bin/sh", "sh", "-c", mw.cmd[i], NULL);
            _exit(127);
        }
        close(fds[1]);
        mw.pid[i] = p;
        mw.fd[i]  = fds[0];
        mw.last[i] = time(NULL);
    }
    for (i = 0; i < mw.n; i++) {
        if (mw.fd[i] < 0) continue;
        fd_set r;
        struct timeval tv;
        char buf[512];
        int n;
        FD_ZERO(&r);
        FD_SET(mw.fd[i], &r);
        tv.tv_sec = 0; tv.tv_usec = 1000;
        if (select(mw.fd[i]+1, &r, NULL, NULL, &tv) <= 0) continue;
        n = read(mw.fd[i], buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            snprintf(line, sizeof line, "\"%s\", %ld: %s",
                     mw.cmd[i], (long)mw.last[i], buf);
            buf_add(t, line);
        }
    }
}

mw_stop(Tab *t)
{
    int i;
    if (!mw.active) return;
    for (i = 0; i < mw.n; i++) {
        if (mw.pid[i] > 0) { kill(mw.pid[i], SIGKILL); waitpid(mw.pid[i], NULL, 0); }
        if (mw.fd[i] >= 0) close(mw.fd[i]);
    }
    mw.active = 0;
    mw.n = 0;
    buf_add(t, "^C\n");
}

/* =====================================================
 *  Run a command, capture output into the tab
 * ===================================================== */
drain_fd(Tab *t, int fd)
{
    char buf[1024];
    int n;
    while ((n = read(fd, buf, sizeof buf - 1)) > 0) {
        buf[n] = 0;
        buf_add(t, buf);
    }
}

run_command(Tab *t, char *line, int background)
{
    char  copy[MAX_LINE];
    char *argv[MAX_ARGS];
    int   argc, fdin = -1, fdout = -1;
    int   i, pid, status;
    char *infile = NULL, *outfile = NULL;
    int   pipefd[2];

    strcpy(copy, line);

    if (!strchr(copy, '|')) {
        for (i = 1; i < (int)strlen(line); i++) {
            if (line[i] == '<') infile = line + i + 1;
            if (line[i] == '>') outfile = line + i + 1;
        }
    }

    argc = split_args(copy, argv);
    if (argc == 0) return 0;
    if (argc > 0 && !strcmp(argv[argc-1], "&")) { argv[argc-1] = NULL; argc--; }

    for (i = 1; i < argc; ) {
        if (!strcmp(argv[i], "<") && i+1 < argc) {
            infile = argv[i+1];
            memmove(&argv[i], &argv[i+2], (argc-i-1)*sizeof(char*));
            argc -= 2;
        } else if (!strcmp(argv[i], ">") && i+1 < argc) {
            outfile = argv[i+1];
            memmove(&argv[i], &argv[i+2], (argc-i-1)*sizeof(char*));
            argc -= 2;
        } else i++;
    }

    if (infile) {
        fdin = open(infile, O_RDONLY);
        if (fdin < 0) {
            char msg[300];
            snprintf(msg, sizeof msg, "terminator: %s: %s\n", infile, strerror(errno));
            buf_add(t, msg);
            return 1;
        }
    }
    if (outfile) {
        fdout = open(outfile, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fdout < 0) {
            char msg[300];
            snprintf(msg, sizeof msg, "terminator: %s: %s\n", outfile, strerror(errno));
            if (fdin >= 0) close(fdin);
            return 1;
        }
    }

    if (pipe(pipefd) < 0) { buf_add(t, "pipe failed\n"); return 1; }
    pid = fork();
    if (pid < 0) { buf_add(t, "fork failed\n"); return 1; }
    if (pid == 0) {
        if (fdin  >= 0) dup2(fdin, 0);
        if (fdout >= 0) dup2(fdout, 1);
        else            dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]); close(pipefd[1]);
        if (fdin  >= 0) close(fdin);
        if (fdout >= 0) close(fdout);
        signal(SIGINT, SIG_DFL);
        execvp(argv[0], argv);
        fprintf(stderr, "terminator: %s: command not found\n", argv[0]);
        _exit(127);
    }
    close(pipefd[1]);
    if (fdin  >= 0) close(fdin);
    if (fdout >= 0) close(fdout);

    if (background) {
        job_add(pid, line);
        t->status = 0;
        close(pipefd[0]);
        return 0;
    }

    t->fg_pid = pid;
    drain_fd(t, pipefd[0]);
    waitpid(pid, &status, 0);
    drain_fd(t, pipefd[0]);
    close(pipefd[0]);
    t->fg_pid = 0;
    if (WIFEXITED(status)) t->status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) t->status = 128 + WTERMSIG(status);
    return t->status;
}

/* Pipeline: capture output of last stage into tab */
run_pipeline(Tab *t, char *line)
{
    char *stages[MAX_ARGS];
    int   nstage = 0, i, prev = -1;
    char *p = line;
    int   cap[2];
    int   last_pid = 0;

    stages[nstage++] = p;
    while (*p) {
        if (*p == '|') { *p = 0; stages[nstage++] = p + 1; }
        p++;
    }

    pipe(cap);

    for (i = 0; i < nstage; i++) {
        int fds[2], pid;
        char *argv[MAX_ARGS];
        int   argc, last = (i == nstage - 1);

        argc = split_args(stages[i], argv);
        if (argc == 0) { close(cap[0]); close(cap[1]); return 1; }
        if (!last) pipe(fds);

        pid = fork();
        if (pid == 0) {
            if (prev >= 0) { dup2(prev, 0); close(prev); }
            if (!last) { close(fds[0]); dup2(fds[1], 1); close(fds[1]); }
            else       { close(cap[0]); dup2(cap[1], 1); dup2(cap[1], 2); close(cap[1]); }
            signal(SIGINT, SIG_DFL);
            execvp(argv[0], argv);
            fprintf(stderr, "terminator: %s: not found\n", argv[0]);
            _exit(127);
        }
        if (prev >= 0) close(prev);
        if (!last) { close(fds[1]); prev = fds[0]; }
        else last_pid = pid;
    }

    close(cap[1]);
    t->fg_pid = last_pid;
    drain_fd(t, cap[0]);
    waitpid(last_pid, NULL, 0);
    drain_fd(t, cap[0]);
    close(cap[0]);
    t->fg_pid = 0;

    /* reap earlier stages */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    t->status = 0;
    return 0;
}

/* =====================================================
 *  Builtins
 * ===================================================== */
exec_builtin(Tab *t, char *line)
{
    char out[1400];
    if (!strcmp(line, "exit")) { running = 0; return 1; }
    if (!strncmp(line, "cd", 2) && (line[2]==0 || line[2]==' ')) {
        char *d = line+2; while (*d == ' ') d++;
        if (!*d) d = getenv("HOME");
        if (chdir(d) != 0) {
            snprintf(out, sizeof out, "cd: %s: %s\n", d, strerror(errno));
            buf_add(t, out); t->status = 1;
        } else { getcwd(t->cwd, sizeof t->cwd); t->status = 0; }
        return 1;
    }
    if (!strcmp(line, "pwd")) {
        snprintf(out, sizeof out, "%s\n", t->cwd); buf_add(t, out); return 1;
    }
    if (!strcmp(line, "history")) {
        int n = histn, start = n > HIST_SHOW ? n - HIST_SHOW : 0, i;
        for (i = start; i < n; i++) {
            snprintf(out, sizeof out, "%5d  %s\n", i+1, hist[i]);
            buf_add(t, out);
        }
        return 1;
    }
    if (!strcmp(line, "jobs")) {
        int i;
        job_reap(t);
        for (i = 0; i < njobs; i++) {
            snprintf(out, sizeof out, "[%d] %s %s\n",
                     jobs[i].id, jobs[i].state==1?"Stopped":"Running", jobs[i].cmd);
            buf_add(t, out);
        }
        return 1;
    }
    if (!strncmp(line, "fg", 2) && (line[2]==0 || line[2]==' ')) {
        int id = 1, i, found = -1;
        char *s = line+2; while (*s == ' ') s++;
        if (*s == '%') s++;
        if (*s) id = atoi(s);
        job_reap(t);
        for (i = 0; i < njobs; i++) if (jobs[i].id == id) { found = i; break; }
        if (found < 0) { buf_add(t, "fg: no such job\n"); return 1; }
        if (jobs[found].state == 1) kill(jobs[found].pid, SIGCONT);
        snprintf(out, sizeof out, "%s\n", jobs[found].cmd); buf_add(t, out);
        t->fg_pid = jobs[found].pid;
        waitpid(jobs[found].pid, NULL, 0);
        t->fg_pid = 0;
        job_reap(t);
        return 1;
    }
    if (!strncmp(line, "bg", 2) && (line[2]==0 || line[2]==' ')) {
        int id = 1, i;
        char *s = line+2; while (*s == ' ') s++;
        if (*s == '%') s++;
        if (*s) id = atoi(s);
        job_reap(t);
        for (i = 0; i < njobs; i++)
            if (jobs[i].id == id) {
                if (jobs[i].state == 1) {
                    kill(jobs[i].pid, SIGCONT);
                    jobs[i].state = 0;
                    snprintf(out, sizeof out, "[%d]+ %s &\n", jobs[i].id, jobs[i].cmd);
                    buf_add(t, out);
                }
                return 1;
            }
            buf_add(t, "bg: no such job\n");
        return 1;
    }
    if (!strncmp(line, "multiWatch", 10)) {
        char *p = line + 10, *q;
        char *cmds[MAX_ARGS];
        int   n = 0;
        while (*p && n < MAX_ARGS) {
            while (*p && (*p == ' ' || *p == '[' || *p == ',' || *p == ']')) p++;
            if (*p != '"') break;
            p++;
            q = strchr(p, '"');
            if (!q) break;
            *q = 0;
            cmds[n++] = p;
            p = q + 1;
        }
        if (n == 0) { buf_add(t, "multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\"]\n"); return 1; }
        mw_start(cmds, n);
        buf_add(t, "multiWatch started (Ctrl+C to stop)\n");
        return 1;
    }
    return 0;
}

/* =====================================================
 *  Simple script interpreter (Task 11)
 * ===================================================== */
run_script(Tab *t, char *path)
{
    FILE *f = fopen(path, "r");
    char line[MAX_LINE];
    int  skip = 0;
    int  last = 0;

    if (!f) {
        char msg[300];
        snprintf(msg, sizeof msg, "source: %s: %s\n", path, strerror(errno));
        buf_add(t, msg); return 1;
    }
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n-1]=='\n' || line[n-1]=='\r')) line[--n] = 0;
        if (!line[0] || line[0] == '#') continue;

        if (!strchr(line, ' ') && strchr(line, '=')) {
            char *eq = strchr(line, '=');
            *eq = 0;
            if (!skip) setenv(line, eq+1, 1);
            continue;
        }
        if (!strncmp(line, "if ", 3)) {
            char *lb = strchr(line, '[');
            if (lb) {
                char a[64], op[8], b[64];
                int ok = 0;
                if (sscanf(lb, "[ %63s %7s %63s ]", a, op, b) == 3) {
                    int ai = atoi(a), bi = atoi(b);
                    if (!strcmp(op, "-eq")) ok = (ai == bi);
                    else if (!strcmp(op, "-gt")) ok = (ai > bi);
                    else if (!strcmp(op, "=")) ok = !strcmp(a, b);
                    else if (!strcmp(op, "-f")) ok = (access(a, F_OK) == 0);
                    else if (!strcmp(op, "-d")) {
                        struct stat st;
                        ok = (stat(a, &st) == 0 && S_ISDIR(st.st_mode));
                    }
                }
                skip = !ok;
            }
            continue;
        }
        if (!strncmp(line, "else", 4)) { skip = !skip; continue; }
        if (!strncmp(line, "fi", 2)) { skip = 0; continue; }
        if (skip) continue;

        if (!strncmp(line, "exit", 4)) {
            int code = 0;
            char *s = line + 4; while (*s == ' ') s++;
            if (*s) code = atoi(s);
            fclose(f);
            t->status = code;
            return code;
        }

        {
            char expanded[MAX_LINE];
            expand(line, expanded, sizeof expanded, t->status);
            if (!exec_builtin(t, expanded))
                run_command(t, expanded, 0);
            last = t->status;
        }
    }
    fclose(f);
    return last;
}

/* =====================================================
 *  Completion / search
 * ===================================================== */
do_complete(Tab *t)
{
    int start, len, n = 0, i, j;
    char prefix[256], names[64][256], lcp[256];
    char *tmp[64];
    DIR *d;
    struct dirent *de;
    char t2[256];

    start = t->cursor;
    while (start > 0 && !isspace((unsigned char)t->input[start-1])) start--;
    len = t->cursor - start;
    if (len >= 256) return;
    memcpy(prefix, t->input + start, len); prefix[len] = 0;

    d = opendir(".");
    if (!d) return;
    while ((de = readdir(d)) && n < 64) {
        if (de->d_name[0] == '.' && prefix[0] != '.') continue;
        if (prefix[0] && strncmp(de->d_name, prefix, strlen(prefix)) != 0) continue;
        strncpy(names[n], de->d_name, 255); names[n][255] = 0;
        tmp[n] = names[n]; n++;
    }
    closedir(d);
    if (n == 0) return;

    for (i = 0; i < n; i++)
        for (j = i+1; j < n; j++)
            if (strcmp(tmp[i], tmp[j]) > 0) { strcpy(t2, tmp[i]); strcpy(tmp[i], tmp[j]); strcpy(tmp[j], t2); }

            if (n == 1) {
                int nl = strlen(names[0]);
                memmove(t->input + start + nl, t->input + start + len,
                        strlen(t->input + start + len) + 1);
                memcpy(t->input + start, names[0], nl);
                t->cursor = start + nl;
                t->inlen = strlen(t->input);
                return;
            }

            strcpy(lcp, names[0]);
            for (i = 1; i < n; i++) {
                int l = 0;
                while (lcp[l] && lcp[l] == names[i][l]) l++;
                lcp[l] = 0;
            }
            if ((int)strlen(lcp) > len) {
                int nl = strlen(lcp);
                memmove(t->input + start + nl, t->input + start + len,
                        strlen(t->input + start + len) + 1);
                memcpy(t->input + start, lcp, nl);
                t->cursor = start + nl;
                t->inlen = strlen(t->input);
                return;
            }
            buf_add(t, "\n");
            for (i = 0; i < n; i++) {
                char line[300];
                snprintf(line, sizeof line, "%d. %s\n", i+1, names[i]);
                buf_add(t, line);
            }
}

run_search(Tab *t)
{
    int i;
    char buf[MAX_LINE];
    for (i = histn-1; i >= 0; i--) {
        if (strstr(hist[i], t->search)) {
            strcpy(t->input, hist[i]); t->inlen = strlen(t->input); t->cursor = t->inlen;
            return;
        }
    }
    buf_add(t, "No match for search term in history\n");
}

/* =====================================================
 *  Execute a line
 * ===================================================== */
execute(Tab *t, char *line)
{
    char expanded[MAX_LINE];

    t->scroll = 0;
    history_add(line);
    expand(line, expanded, sizeof expanded, t->status);

    if (exec_builtin(t, expanded)) return;

    if (!strncmp(expanded, "source ", 7)) { run_script(t, expanded + 7); return; }
    if (!strncmp(expanded, "bash ", 5))   { run_script(t, expanded + 5); return; }

    if (strchr(expanded, '|')) run_pipeline(t, expanded);
    else {
        int bg = 0;
        size_t n = strlen(expanded);
        if (n && expanded[n-1] == '&') { expanded[n-1] = 0; bg = 1; }
        run_command(t, expanded, bg);
    }
}

/* =====================================================
 *  Keyboard
 * ===================================================== */
handle_key(Tab *t, KeySym key, char *text, int len, unsigned state)
{
    if (t->search_mode) {
        if (key == XK_Return) {
            t->search_mode = 0; run_search(t);
            t->search_len = 0; t->search[0] = 0;
        } else if (key == XK_BackSpace) {
            if (t->search_len) t->search[--t->search_len] = 0;
        } else if (len > 0 && t->search_len < MAX_LINE-1) {
            t->search[t->search_len++] = text[0];
            t->search[t->search_len] = 0;
        }
        return;
    }

    if (key == XK_Return) {
        t->input[t->inlen] = 0;
        if (needs_more(t->input)) {
            /* strip trailing '\' (shell line continuation) */
            if (t->inlen > 0 && t->input[t->inlen-1] == '\\') {
                t->input[t->inlen-1] = 0;
                t->inlen--;
            }
            strncat(t->multi, t->input, sizeof t->multi - strlen(t->multi) - 1);
            strcat(t->multi, "\n");
            t->multiline = 1;
            t->inlen = 0; t->cursor = 0; t->input[0] = 0;
            return;
        }
        {
            char full[MAX_LINE*2];
            strcpy(full, t->multi);
            strncat(full, t->input, sizeof full - strlen(full) - 1);
            t->multi[0] = 0; t->multiline = 0;
            t->inlen = 0; t->cursor = 0; t->input[0] = 0;
            if (*full) {
                char echo[MAX_LINE * 2 + 8];
                snprintf(echo, sizeof echo, "$ %s\n", full);
                buf_add(t, echo);
                execute(t, full);
            }
        }
        prompt(t);
        return;
    }

    if (key == XK_BackSpace) {
        if (t->cursor > 0) {
            memmove(t->input + t->cursor-1, t->input + t->cursor,
                    t->inlen - t->cursor + 1);
            t->cursor--; t->inlen--;
        }
        return;
    }
    if (key == XK_Left)  { if (t->cursor > 0) t->cursor--; return; }
    if (key == XK_Right) { if (t->cursor < t->inlen) t->cursor++; return; }
    if (key == XK_Page_Up)   { t->scroll += 10; return; }
    if (key == XK_Page_Down) { t->scroll -= 10; if (t->scroll < 0) t->scroll = 0; return; }
    if (key == XK_Tab) { do_complete(t); return; }

    if ((state & ControlMask) && key >= XK_1 && key <= XK_8) {
        int i = key - XK_1;
        if (i < MAX_TABS && tabs[i].active) cur_tab = i;
        return;
    }
    if ((state & ControlMask) && key == XK_t) {
        int i;
        for (i = 0; i < MAX_TABS; i++) {
            if (!tabs[i].active) {
                memset(&tabs[i], 0, sizeof(Tab));
                tabs[i].active = 1;
                getcwd(tabs[i].cwd, sizeof tabs[i].cwd);
                cur_tab = i;
                prompt(&tabs[i]);
                break;
            }
        }
        return;
    }
    if ((state & ControlMask) && key == XK_w) {
        int i;
        t->active = 0;
        for (i = 0; i < MAX_TABS; i++) if (tabs[i].active) { cur_tab = i; return; }
        running = 0;
        return;
    }
    if ((state & ControlMask) && key == XK_c) {
        if (mw.active) { mw_stop(t); prompt(t); return; }
        if (t->fg_pid > 0) kill(t->fg_pid, SIGINT);
        buf_add(t, "^C\n");
        prompt(t);
        return;
    }
    if ((state & ControlMask) && key == XK_z) {
        if (t->fg_pid > 0) {
            kill(t->fg_pid, SIGTSTP);
            job_add(t->fg_pid, "stopped command");
        }
        return;
    }
    if ((state & ControlMask) && key == XK_a) { t->cursor = 0; return; }
    if ((state & ControlMask) && key == XK_e) { t->cursor = t->inlen; return; }
    if ((state & ControlMask) && key == XK_u) {
        memmove(t->input, t->input + t->cursor, t->inlen - t->cursor + 1);
        t->inlen -= t->cursor;
        t->cursor = 0;
        return;
    }
    if ((state & ControlMask) && key == XK_k) {
        t->input[t->cursor] = 0; t->inlen = t->cursor; return;
    }
    if ((state & ControlMask) && key == XK_l) { t->screen[0] = 0; return; }
    if ((state & ControlMask) && key == XK_d) {
        if (t->inlen == 0) running = 0;
        return;
    }
    if ((state & ControlMask) && key == XK_r) {
        t->search_mode = 1;
        t->search_len = 0;
        t->search[0] = 0;
        return;
    }
    if (key == XK_Up) {
        int i;
        for (i = histn-1; i >= 0; i--) {
            if (strcmp(hist[i], t->input) != 0) {
                strcpy(t->input, hist[i]); t->inlen = strlen(t->input); t->cursor = t->inlen;
                return;
            }
        }
        return;
    }
    if (key == XK_Down) { t->input[0] = 0; t->inlen = 0; t->cursor = 0; return; }

    if (len > 0 && t->inlen < MAX_LINE-2) {
        memmove(t->input + t->cursor + 1, t->input + t->cursor,
                t->inlen - t->cursor + 1);
        t->input[t->cursor] = text[0];
        t->cursor++; t->inlen++;
    }
}

/* =====================================================
 *  Focus helper (fixes BadMatch)
 * ===================================================== */
focus_window()
{
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, win, &a)) return;
    if (a.map_state == IsViewable)
        XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
}

/* =====================================================
 *  Init + main
 * ===================================================== */
init_ui()
{
    int i;
    for (i = 0; i < MAX_TABS; i++) {
        memset(&tabs[i], 0, sizeof(Tab));
        tabs[i].active = 0;
    }
    tabs[0].active = 1;
    getcwd(tabs[0].cwd, sizeof tabs[0].cwd);
    prompt(&tabs[0]);

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open display\n"); exit(1); }
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
                              10, 10, 900, 600, 1,
                              BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask |
    FocusChangeMask | StructureNotifyMask);
    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, scr));
    XSetBackground(dpy, gc, WhitePixel(dpy, scr));
}

main(int argc, char **argv)
{
    int i;
    XEvent event;
    KeySym key;
    char text[32];
    int len;

    for (i = 1; i < argc; i++)
        if (!strncmp(argv[i], "useProcess", 10)) use_process = 1;

        init_ui();

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &event);
            if (event.type == Expose) redraw();
            else if (event.type == MapNotify) focus_window();
            else if (event.type == ButtonPress) {
                focus_window();
                if (event.xbutton.button == Button4) { tabs[cur_tab].scroll += 3; redraw(); }
                else if (event.xbutton.button == Button5) {
                    tabs[cur_tab].scroll -= 3;
                    if (tabs[cur_tab].scroll < 0) tabs[cur_tab].scroll = 0;
                    redraw();
                }
            } else if (event.type == KeyPress) {
                key = NoSymbol;
                len = XLookupString(&event.xkey, text, sizeof text, &key, NULL);
                if (key == NoSymbol && len == 0) continue;
                handle_key(&tabs[cur_tab], key, text, len, event.xkey.state);
                redraw();
            }
        }
        mw_tick(&tabs[cur_tab]);
        job_reap(&tabs[cur_tab]);
        usleep(10000);
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
