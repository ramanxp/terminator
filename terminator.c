#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <semaphore.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#define MAX_LINE   4096
#define MAX_TABS   8
#define MAX_ARGS   128
#define MAX_HIST   10000
#define HIST_SHOW  1000
#define SCR_BUF    131072
#define VISIBLE    26
#define MAX_JOBS   64
#define MAX_CMDS   16
#define MAX_BIND   64
#define QSIZE      8
#define MW_WAIT    2

typedef struct { int id; pid_t pid; int state; char cmd[MAX_LINE]; } Job;

typedef struct {
    sem_t lock;
    int   count;
    char  lines[MAX_HIST][MAX_LINE];
} Hist;

typedef struct {
    sem_t lock;              /* protects screen, queue, cwd, status */
    sem_t wake;
    int   alive;

    /* command queue */
    char  queue[QSIZE][MAX_LINE];
    int   qhead, qtail, qcount;

    /* output */
    char  screen[SCR_BUF];

    /* shared shell state */
    char  cwd[PATH_MAX];
    int   status;
    volatile pid_t fg_pid;
    volatile int   mw_active;

    /* job list */
    Job   jobs[MAX_JOBS];
    int   njobs;

    /* multiWatch bookkeeping (worker-only) */
    pid_t mw_pids[MAX_CMDS];
    int   mw_fds[MAX_CMDS];
    time_t mw_started[MAX_CMDS];
    int   mw_state[MAX_CMDS];   /* 0 idle, 1 running */

    /* GUI-only state */
    char  input[MAX_LINE];
    int   inlen, cursor;
    int   multiline;
    char  multi[MAX_LINE * 2];
    int   search_mode;
    char  search[MAX_LINE];
    int   search_len;
    int   scroll;
} Tab;

typedef struct { char name[32]; KeySym ks; unsigned mods; } Bind;

static Hist  *H;
static Tab   *S;
static int    cur_tab = 0;
static int    use_process = 0;
static int    running = 1;
static pthread_t threads[MAX_TABS];

static Display *dpy;
static Window   win;
static GC       gc;
static int      scr;

static Bind binds[MAX_BIND];
static int  nbind = 0;

/* ======================================================================
 * History
 * ====================================================================== */
history_init()
{
    char path[512], buf[MAX_LINE];
    const char *home = getenv("HOME");
    FILE *f; size_t n;

    H = mmap(NULL, sizeof(Hist), PROT_READ|PROT_WRITE,
             MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    sem_init(&H->lock, 1, 1);
    H->count = 0;
    if (!home) return;
    snprintf(path, sizeof path, "%s/.terminator_history", home);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(buf, sizeof buf, f)) {
        n = strlen(buf);
        while (n && (buf[n-1]=='\n' || buf[n-1]=='\r')) buf[--n] = 0;
        if (!n) continue;
        if (H->count < MAX_HIST) {
            strcpy(H->lines[H->count], buf);
            H->count++;
        }
    }
    fclose(f);
}

history_add(char *line)
{
    char path[512];
    const char *home = getenv("HOME");
    FILE *f; int i;

    if (!line || !*line) return;
    sem_wait(&H->lock);
    if (H->count < MAX_HIST) {
        strncpy(H->lines[H->count], line, MAX_LINE-1);
        H->lines[H->count][MAX_LINE-1] = 0;
        H->count++;
    } else {
        for (i = 0; i < MAX_HIST-1; i++) strcpy(H->lines[i], H->lines[i+1]);
        strncpy(H->lines[MAX_HIST-1], line, MAX_LINE-1);
        H->lines[MAX_HIST-1][MAX_LINE-1] = 0;
    }
    sem_post(&H->lock);

    if (!home) return;
    snprintf(path, sizeof path, "%s/.terminator_history", home);
    f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

history_get(int idx, char *out)
{
    sem_wait(&H->lock);
    if (idx >= 0 && idx < H->count) strcpy(out, H->lines[idx]);
    else out[0] = 0;
    sem_post(&H->lock);
}

/* ======================================================================
 * Screen buffer
 * ====================================================================== */
scr_add(Tab *s, char *txt)
{
    int used, space, n;
    sem_wait(&s->lock);
    used  = strlen(s->screen);
    space = SCR_BUF - used - 1;
    n     = strlen(txt);
    if (n > space) {
        char *nl = strchr(s->screen, '\n');
        if (nl) {
            int drop = nl - s->screen + 1;
            memmove(s->screen, s->screen + drop, used - drop + 1);
            used -= drop; space = SCR_BUF - used - 1;
        }
    }
    if (n > space) n = space;
    if (n > 0) { memcpy(s->screen + used, txt, n); s->screen[used+n] = 0; }
    sem_post(&s->lock);
}

scr_clear(Tab *s) { sem_wait(&s->lock); s->screen[0] = 0; sem_post(&s->lock); }

/* ======================================================================
 * Bindings
 * ====================================================================== */
load_binds()
{
    char path[512], nm[32], kn[32], mn[32];
    const char *home = getenv("HOME");
    FILE *f; int i;
    static struct { char *n; KeySym k; unsigned m; } defs[] = {
        {"clear", XK_l, ControlMask}, {"exit", XK_d, ControlMask},
        {"home",  XK_a, ControlMask}, {"end",  XK_e, ControlMask},
        {"kill_word", XK_w, ControlMask}, {"search", XK_r, ControlMask},
        {"up", XK_Up, 0}, {"down", XK_Down, 0}, {NULL,0,0}
    };
    for (i = 0; defs[i].n; i++) {
        strcpy(binds[nbind].name, defs[i].n);
        binds[nbind].ks = defs[i].k; binds[nbind].mods = defs[i].m; nbind++;
    }
    if (!home) return;
    snprintf(path, sizeof path, "%s/.terminatorrc", home);
    f = fopen(path, "r"); if (!f) return;
    while (fscanf(f, "%31s %31s %31s", nm, kn, mn) == 3) {
        int idx = -1; KeySym k = XStringToKeysym(kn);
        unsigned m = (!strcmp(mn,"ctrl")) ? ControlMask : 0;
        for (i = 0; i < nbind; i++) if (!strcmp(binds[i].name, nm)) { idx = i; break; }
        if (idx < 0 && nbind < MAX_BIND) { strcpy(binds[nbind].name, nm); idx = nbind++; }
        if (idx >= 0 && k != NoSymbol) { binds[idx].ks = k; binds[idx].mods = m; }
    }
    fclose(f);
}

char *bind_for(KeySym k, unsigned mods)
{
    int i;
    for (i = 0; i < nbind; i++)
        if (binds[i].ks == k && binds[i].mods == (mods & (ControlMask|ShiftMask)))
            return binds[i].name;
    return NULL;
}

/* ======================================================================
 * Parsing helpers
 * ====================================================================== */
needs_more(char *line)
{
    int sq=0, dq=0, bt=0, par=0, n;
    char *p;
    for (p = line; *p; p++) {
        if (*p=='\\' && !sq) { p++; continue; }
        if (*p=='\'' && !dq) { sq=!sq; continue; }
        if (*p=='"'  && !sq) { dq=!dq; continue; }
        if (*p=='`'  && !sq) { bt=!bt; continue; }
        if (*p=='$' && p[1]=='(' && !sq) { par++; p++; continue; }
        if (*p==')' && !sq && par) { par--; continue; }
    }
    if (sq||dq||bt||par) return 1;
    n = strlen(line);
    if (n && line[n-1]=='\\') return 1;
    return 0;
}

expand(Tab *s, char *in, char *out, int cap)
{
    char *p = in, *q, *v, name[64], inner[1024], buf[256];
    int o=0, sq=0, dq=0, depth, il, nl;
    FILE *fp; size_t n;

    while (*p && o < cap-1) {
        if (*p=='\'' && !dq) { sq=!sq; out[o++]=*p++; continue; }
        if (*p=='"'  && !sq) { dq=!dq; out[o++]=*p++; continue; }
        if (*p=='\\' && !sq) {
            char nx = p[1];
            if (nx=='\\'||nx=='$'||nx=='`'||nx=='"'||nx=='\''||nx=='\n'||nx==' ') {
                p++; if (o<cap-1) out[o++] = *p++;
            } else if (o<cap-1) out[o++] = *p++;
            continue;
        }
        if (*p=='$' && !sq) {
            if (p[1]=='?') { o += snprintf(out+o, cap-o, "%d", s->status); p+=2; continue; }
            if (p[1]=='#') { o += snprintf(out+o, cap-o, "%d", 0); p+=2; continue; }
            if (isdigit((unsigned char)p[1])) {
                int i=0; q=p+1;
                while (isdigit((unsigned char)*q) && i<63) name[i++]=*q++;
                name[i]=0;
                v = getenv(name);
                if (v) { int vl=strlen(v); if (o+vl<cap-1) { memcpy(out+o,v,vl); o+=vl; } }
                p = q; continue;
            }
            if (p[1]=='(') {
                depth=1; q=p+2;
                while (*q && depth) {
                    if (*q=='(') depth++;
                    else if (*q==')') depth--;
                    if (depth) q++;
                }
                if (!*q) { out[o++]=*p++; continue; }
                il = q-(p+2); if (il >= (int)sizeof inner) il = sizeof inner - 1;
                memcpy(inner, p+2, il); inner[il]=0;
                fp = popen(inner, "r");
                if (fp) {
                    while ((n = fread(buf,1,sizeof buf,fp))>0)
                        if (o+(int)n<cap-1) { memcpy(out+o,buf,n); o+=n; }
                        pclose(fp);
                }
                while (o && out[o-1]=='\n') o--;
                p = q+1; continue;
            }
            if (isalpha((unsigned char)p[1]) || p[1]=='_') {
                nl=0; q=p+1;
                while ((isalnum((unsigned char)*q)||*q=='_') && nl<63) name[nl++]=*q++;
                name[nl]=0;
                v = getenv(name);
                if (v) { int vl=strlen(v); if (o+vl<cap-1) { memcpy(out+o,v,vl); o+=vl; } }
                p = q; continue;
            }
        }
        if (*p=='`' && !sq) {
            q = strchr(p+1, '`');
            if (q) {
                il = q-(p+1); if (il >= (int)sizeof inner) il = sizeof inner - 1;
                memcpy(inner, p+1, il); inner[il]=0;
                fp = popen(inner, "r");
                if (fp) {
                    while ((n = fread(buf,1,sizeof buf,fp))>0)
                        if (o+(int)n<cap-1) { memcpy(out+o,buf,n); o+=n; }
                        pclose(fp);
                }
                while (o && out[o-1]=='\n') o--;
                p = q+1; continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

split_args(char *line, char **argv, int max)
{
    char buf[MAX_LINE]; int bl, n = 0, sq, dq;
    char *p = line;
    while (*p && n < max-1) {
        while (*p==' '||*p=='\t'||*p=='\n') p++;
        if (!*p) break;
        bl=0; sq=0; dq=0;
        while (*p && (sq || dq || (*p!=' ' && *p!='\t' && *p!='\n'))) {
            if (*p=='\\' && !sq && p[1]) { buf[bl++]=p[1]; p+=2; continue; }
            if (*p=='\'' && !dq) { sq=!sq; p++; continue; }
            if (*p=='"'  && !sq) { dq=!dq; p++; continue; }
            buf[bl++] = *p++;
        }
        buf[bl]=0; argv[n++] = strdup(buf);
    }
    argv[n]=NULL;
    return n;
}

free_argv(char **argv, int n) { int i; for (i = 0; i < n; i++) free(argv[i]); }

/* ======================================================================
 * Builtins
 * ====================================================================== */
builtin_cd(Tab *s, char *arg)
{
    char path[PATH_MAX], real[PATH_MAX];
    struct stat st;
    if (!arg || !*arg) arg = getenv("HOME");
    if (!arg) arg = "/";
    if (arg[0]=='~') {
        const char *h = getenv("HOME");
        if (h) { snprintf(path, sizeof path, "%s%s", h, arg+1); arg = path; }
    }
    if (arg[0]=='/') snprintf(path, sizeof path, "%s", arg);
    else snprintf(path, sizeof path, "%s/%s", s->cwd, arg);
    if (stat(path, &st) < 0) {
        char m[400]; snprintf(m, sizeof m, "cd: %s: %s\n", arg, strerror(errno)); scr_add(s, m);
        s->status = 1; return;
    }
    if (!S_ISDIR(st.st_mode)) { scr_add(s, "cd: not a directory\n"); s->status = 1; return; }
    if (!realpath(path, real)) { s->status = 1; return; }
    sem_wait(&s->lock);
    strncpy(s->cwd, real, sizeof s->cwd - 1);
    s->cwd[sizeof s->cwd - 1] = 0;
    s->status = 0;
    sem_post(&s->lock);
}

builtin_pwd(Tab *s) { char m[PATH_MAX+4]; snprintf(m, sizeof m, "%s\n", s->cwd); scr_add(s, m); s->status = 0; }

builtin_history(Tab *s)
{
    int n, start, i;
    char buf[MAX_LINE], m[MAX_LINE+16];
    sem_wait(&H->lock); n = H->count; sem_post(&H->lock);
    start = n > HIST_SHOW ? n - HIST_SHOW : 0;
    for (i = start; i < n; i++) {
        history_get(i, buf);
        snprintf(m, sizeof m, "%5d  %s\n", i+1, buf);
        scr_add(s, m);
    }
    s->status = 0;
}

job_add(Tab *s, pid_t pid, char *cmd)
{
    int i, cand, used;
    if (s->njobs >= MAX_JOBS) return;
    s->jobs[s->njobs].id = 0;
    for (cand = 1; cand <= MAX_JOBS; cand++) {
        used = 0;
        for (i = 0; i < s->njobs; i++) if (s->jobs[i].id == cand) { used = 1; break; }
        if (!used) { s->jobs[s->njobs].id = cand; break; }
    }
    s->jobs[s->njobs].pid = pid;
    s->jobs[s->njobs].state = 0;
    strncpy(s->jobs[s->njobs].cmd, cmd, MAX_LINE-1);
    s->jobs[s->njobs].cmd[MAX_LINE-1] = 0;
    s->njobs++;
}

job_reap(Tab *s)
{
    int i, w, st;
    for (i = 0; i < s->njobs; i++) {
        if (waitpid(s->jobs[i].pid, &st, WNOHANG|WUNTRACED|WCONTINUED) <= 0) continue;
        if (WIFEXITED(st) || WIFSIGNALED(st)) s->jobs[i].state = 2;
        else if (WIFSTOPPED(st)) s->jobs[i].state = 1;
        else if (WIFCONTINUED(st)) s->jobs[i].state = 0;
    }
    w = 0;
    for (i = 0; i < s->njobs; i++) {
        if (s->jobs[i].state == 2) continue;
        if (w != i) s->jobs[w] = s->jobs[i];
        w++;
    }
    s->njobs = w;
}

builtin_jobs(Tab *s)
{
    int i; char m[MAX_LINE+32];
    job_reap(s);
    for (i = 0; i < s->njobs; i++) {
        snprintf(m, sizeof m, "[%d] %s %s\n",
                 s->jobs[i].id, s->jobs[i].state==1?"Stopped":"Running", s->jobs[i].cmd);
        scr_add(s, m);
    }
    s->status = 0;
}

builtin_fg(Tab *s, int id)
{
    int i, st; char m[MAX_LINE];
    job_reap(s);
    for (i = 0; i < s->njobs; i++) {
        if (s->jobs[i].id != id) continue;
        if (s->jobs[i].state == 1) kill(s->jobs[i].pid, SIGCONT);
        snprintf(m, sizeof m, "%s\n", s->jobs[i].cmd); scr_add(s, m);
        s->fg_pid = s->jobs[i].pid;
        waitpid(s->jobs[i].pid, &st, WUNTRACED);
        s->fg_pid = 0;
        s->jobs[i].state = WIFSTOPPED(st) ? 1 : 2;
        job_reap(s);
        s->status = WIFEXITED(st) ? WEXITSTATUS(st) : 0;
        return;
    }
    scr_add(s, "fg: no such job\n"); s->status = 1;
}

builtin_bg(Tab *s, int id)
{
    int i; char m[MAX_LINE+32];
    job_reap(s);
    for (i = 0; i < s->njobs; i++) {
        if (s->jobs[i].id != id) continue;
        if (s->jobs[i].state == 1) {
            kill(s->jobs[i].pid, SIGCONT);
            s->jobs[i].state = 0;
            snprintf(m, sizeof m, "[%d]+ %s &\n", s->jobs[i].id, s->jobs[i].cmd); scr_add(s, m);
        }
        s->status = 0; return;
    }
    scr_add(s, "bg: no such job\n"); s->status = 1;
}

/* ======================================================================
 * Run one simple command
 * ====================================================================== */
run_simple(Tab *s, char *line, int bg)
{
    char *argv[MAX_ARGS], buf[1024], msg[MAX_LINE];
    char *in = NULL, *out = NULL;
    int  argc, i, pid, fd[2], fdin = -1, fdout = -1, status, n;

    argc = split_args(line, argv, MAX_ARGS);
    if (argc == 0) return 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"<") && i+1 < argc) { in = argv[i+1]; argv[i] = NULL; argv[i+1] = NULL; argc = i; break; }
        if (!strcmp(argv[i],">") && i+1 < argc) { out = argv[i+1]; argv[i] = NULL; argv[i+1] = NULL; argc = i; break; }
    }
    if (argc == 0) return 0;

    if (in) {
        fdin = open(in, O_RDONLY);
        if (fdin < 0) {
            snprintf(msg, sizeof msg, "terminator: %s: %s\n", in, strerror(errno));
            scr_add(s, msg); free_argv(argv, argc); return 1;
        }
    }
    if (out) {
        fdout = open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fdout < 0) {
            snprintf(msg, sizeof msg, "terminator: %s: %s\n", out, strerror(errno));
            if (fdin >= 0) close(fdin); free_argv(argv, argc); return 1;
        }
    }

    pipe(fd);
    pid = fork();
    if (pid == 0) {
        chdir(s->cwd);
        if (fdin >= 0) { dup2(fdin,0); close(fdin); }
        if (fdout >= 0) { dup2(fdout,1); dup2(fdout,2); close(fdout); }
        else { dup2(fd[1],1); dup2(fd[1],2); }
        close(fd[0]); close(fd[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "terminator: %s: not found\n", argv[0]);
        _exit(127);
    }
    close(fd[1]);
    free_argv(argv, argc);
    if (fdin >= 0) close(fdin);
    if (fdout >= 0) close(fdout);

    if (bg) {
        close(fd[0]);
        sem_wait(&s->lock); job_add(s, pid, line); sem_post(&s->lock);
        s->status = 0;
        return 0;
    }
    s->fg_pid = pid;
    while ((n = read(fd[0], buf, sizeof buf - 1)) > 0) { buf[n] = 0; scr_add(s, buf); }
    waitpid(pid, &status, WUNTRACED);
    s->fg_pid = 0;
    if (WIFEXITED(status))        s->status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) s->status = 128 + WTERMSIG(status);
    else if (WIFSTOPPED(status)) {
        sem_wait(&s->lock);
        job_add(s, pid, line);
        s->jobs[s->njobs-1].state = 1;
        snprintf(msg, sizeof msg, "\n[%d]+ Stopped %s\n", s->jobs[s->njobs-1].id, line);
        sem_post(&s->lock);
        scr_add(s, msg);
    }
    close(fd[0]);
    return s->status;
}

/* ======================================================================
 * Pipeline
 * ====================================================================== */
run_pipeline(Tab *s, char *line)
{
    char *stages[MAX_CMDS], cwd[PATH_MAX], buf[1024];
    int nst = 0, i, prev = -1, pid, fd[2], cap[2], status, lastpid = 0, n;
    char *p = line, *argv[MAX_ARGS];

    stages[nst++] = p;
    while (*p) { if (*p=='|') { *p = 0; stages[nst++] = p+1; } p++; }

    strcpy(cwd, s->cwd);
    pipe(cap);
    for (i = 0; i < nst; i++) {
        int argc, last = (i == nst-1);
        argc = split_args(stages[i], argv, MAX_ARGS);
        if (argc == 0) { close(cap[0]); close(cap[1]); return 1; }
        if (!last) pipe(fd);
        pid = fork();
        if (pid == 0) {
            chdir(cwd);
            if (prev >= 0) { dup2(prev,0); close(prev); }
            if (!last) { close(fd[0]); dup2(fd[1],1); close(fd[1]); }
            else { close(cap[0]); dup2(cap[1],1); dup2(cap[1],2); close(cap[1]); }
            execvp(argv[0], argv);
            fprintf(stderr, "terminator: %s: not found\n", argv[0]);
            _exit(127);
        }
        free_argv(argv, argc);
        if (prev >= 0) close(prev);
        if (!last) { close(fd[1]); prev = fd[0]; }
        else lastpid = pid;
    }
    close(cap[1]);
    s->fg_pid = lastpid;
    while ((n = read(cap[0], buf, sizeof buf - 1)) > 0) { buf[n] = 0; scr_add(s, buf); }
    waitpid(lastpid, &status, 0);
    s->fg_pid = 0;
    close(cap[0]);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    s->status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return s->status;
}

/* ======================================================================
 * multiWatch (Task 6)
 * ====================================================================== */
parse_mw(char *line, char **cmds, int *np)
{
    char *p = line, *q; int n = 0;
    while (*p && n < MAX_CMDS) {
        while (*p && (*p==' '||*p=='['||*p==','||*p==']')) p++;
        if (*p != '"') break;
        p++; q = strchr(p, '"');
        if (!q) break;
        *q = 0;
        cmds[n++] = strdup(p);
        p = q+1;
    }
    *np = n;
}

run_multiwatch(Tab *s, char *line)
{
    char *cmds[MAX_CMDS]; int n, i, rn, st;
    time_t now;
    char out[MAX_LINE], msg[MAX_LINE*2];

    parse_mw(line, cmds, &n);
    if (n == 0) { scr_add(s, "usage: multiWatch [\"cmd1\", \"cmd2\"]\n"); return 1; }

    s->mw_active = 1;
    for (i = 0; i < n; i++) { s->mw_pids[i] = 0; s->mw_fds[i] = -1; s->mw_started[i] = 0; s->mw_state[i] = 0; }

    while (s->mw_active) {
        now = time(NULL);
        /* reap finished */
        for (i = 0; i < n; i++) {
            if (s->mw_state[i] == 1) {
                if (waitpid(s->mw_pids[i], &st, WNOHANG) == s->mw_pids[i]) {
                    s->mw_state[i] = 0;
                    if (s->mw_fds[i] >= 0) { close(s->mw_fds[i]); s->mw_fds[i] = -1; }
                    s->mw_started[i] = now;
                }
            }
        }
        /* start due */
        for (i = 0; i < n; i++) {
            if (s->mw_state[i] != 0) continue;
            if (now - s->mw_started[i] < MW_WAIT && s->mw_started[i] != 0) continue;
            {
                int fd[2], pid;
                pipe(fd);
                pid = fork();
                if (pid == 0) {
                    close(fd[0]);
                    dup2(fd[1],1); dup2(fd[1],2); close(fd[1]);
                    execl("/bin/sh", "sh", "-c", cmds[i], NULL);
                    _exit(127);
                }
                close(fd[1]);
                s->mw_pids[i] = pid; s->mw_fds[i] = fd[0];
                s->mw_state[i] = 1; s->mw_started[i] = now;
            }
        }
        /* drain output */
        for (i = 0; i < n; i++) {
            fd_set r; struct timeval tv;
            if (s->mw_fds[i] < 0) continue;
            FD_ZERO(&r); FD_SET(s->mw_fds[i], &r);
            tv.tv_sec = 0; tv.tv_usec = 1000;
            if (select(s->mw_fds[i]+1, &r, NULL, NULL, &tv) <= 0) continue;
            rn = read(s->mw_fds[i], out, sizeof out - 1);
            if (rn > 0) {
                out[rn] = 0;
                snprintf(msg, sizeof msg, "\"%s\", %ld: %s", cmds[i], (long)s->mw_started[i], out);
                scr_add(s, msg);
            }
        }
        usleep(50000);
    }

    /* stop all */
    for (i = 0; i < n; i++) {
        if (s->mw_state[i] == 1) { kill(s->mw_pids[i], SIGKILL); waitpid(s->mw_pids[i], NULL, 0); }
        if (s->mw_fds[i] >= 0) { close(s->mw_fds[i]); s->mw_fds[i] = -1; }
        s->mw_state[i] = 0;
        free(cmds[i]);
    }
    scr_add(s, "^C\n");
    return 0;
}

/* ======================================================================
 * Script interpreter (Task 11)
 * ====================================================================== */
run_script(Tab *s, char *path, char **args, int nargs)
{
    char full[PATH_MAX], line[MAX_LINE], expanded[MAX_LINE];
    char envname[16];
    FILE *f;
    int depth = 0, if_taken[64] = {0}, skip = 0, last = 0, i;

    if (path[0]=='/') snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "%s/%s", s->cwd, path);
    f = fopen(full, "r");
    if (!f) {
        char m[400]; snprintf(m, sizeof m, "source: %s: %s\n", path, strerror(errno));
        scr_add(s, m); return 1;
    }
    /* set $1..$N */
    for (i = 0; i < nargs; i++) { snprintf(envname, sizeof envname, "%d", i+1); setenv(envname, args[i], 1); }
    { char b[8]; snprintf(b, sizeof b, "%d", nargs); setenv("#", b, 1); }

    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L-1]=='\n' || line[L-1]=='\r')) line[--L] = 0;
        if (!line[0] || line[0]=='#') continue;

        if (!strncmp(line, "if ", 3)) {
            char *lb = strchr(line, '[');
            int ok = 0;
            if (lb) {
                char a[64], op[8], b[64];
                if (sscanf(lb, "[ %63s %7s %63s ]", a, op, b) == 3) {
                    int ai = atoi(a), bi = atoi(b);
                    if (!strcmp(op,"-eq")) ok = ai==bi;
                    else if (!strcmp(op,"-gt")) ok = ai>bi;
                    else if (!strcmp(op,"="))   ok = !strcmp(a,b);
                    else if (!strcmp(op,"-f"))  ok = access(a,F_OK)==0;
                    else if (!strcmp(op,"-d")) { struct stat st; ok = stat(a,&st)==0 && S_ISDIR(st.st_mode); }
                }
            }
            if (depth < 64) { if_taken[depth] = ok; depth++; }
            skip = !ok; continue;
        }
        if (!strncmp(line, "elif ", 5)) {
            if (depth == 0) { skip = 0; continue; }
            if (if_taken[depth-1]) { skip = 1; continue; }
            {
                char *lb = strchr(line, '[');
                int ok = 0;
                if (lb) {
                    char a[64], op[8], b[64];
                    if (sscanf(lb, "[ %63s %7s %63s ]", a, op, b) == 3) {
                        int ai = atoi(a), bi = atoi(b);
                        if (!strcmp(op,"-eq")) ok = ai==bi;
                        else if (!strcmp(op,"-gt")) ok = ai>bi;
                        else if (!strcmp(op,"="))   ok = !strcmp(a,b);
                        else if (!strcmp(op,"-f"))  ok = access(a,F_OK)==0;
                    }
                }
                if_taken[depth-1] = ok; skip = !ok;
            }
            continue;
        }
        if (!strncmp(line, "else", 4)) {
            if (depth > 0) { if (if_taken[depth-1]) skip = 1; else { skip = 0; if_taken[depth-1] = 1; } }
            continue;
        }
        if (!strncmp(line, "fi", 2)) {
            if (depth > 0) depth--;
            skip = depth > 0 ? !if_taken[depth-1] : 0;
            continue;
        }
        if (skip) continue;

        if (!strncmp(line, "exit", 4)) {
            int code = 0; char *c = line+4;
            while (*c == ' ') c++;
            if (*c) code = atoi(c);
            fclose(f); s->status = code; return code;
        }

        expand(s, line, expanded, sizeof expanded);
        if (!strcmp(expanded, "pwd")) { builtin_pwd(s); last = 0; continue; }
        if (!strncmp(expanded, "cd ", 3)) { builtin_cd(s, expanded+3); last = s->status; continue; }
        if (!strncmp(expanded, "source ", 7)) { run_script(s, expanded+7, NULL, 0); last = s->status; continue; }
        if (strchr(expanded, '|')) { run_pipeline(s, expanded); last = s->status; continue; }
        run_simple(s, expanded, 0);
        last = s->status;
    }
    fclose(f);
    if (depth > 0) scr_add(s, "terminator: script: missing fi\n");
    return last;
}

/* ======================================================================
 * Command dispatcher (worker side)
 * ====================================================================== */
exec_line(Tab *s, char *line)
{
    char expanded[MAX_LINE];

    history_add(line);
    expand(s, line, expanded, sizeof expanded);

    if (!strcmp(expanded, "exit")) { running = 0; s->status = 0; return 0; }
    if (!strcmp(expanded, "pwd"))     { builtin_pwd(s); return 0; }
    if (!strcmp(expanded, "history")) { builtin_history(s); return 0; }
    if (!strcmp(expanded, "jobs"))    { builtin_jobs(s); return 0; }
    if (!strncmp(expanded,"cd",2) && (expanded[2]==0 || expanded[2]==' ')) {
        char *a = expanded+2; while (*a==' ') a++;
        builtin_cd(s, a); return 0;
    }
    if (!strncmp(expanded,"fg",2) && (expanded[2]==0 || expanded[2]==' ')) {
        int id = 1; char *a = expanded+2;
        while (*a==' ') a++; if (*a=='%') a++; if (*a) id = atoi(a);
        builtin_fg(s, id); return 0;
    }
    if (!strncmp(expanded,"bg",2) && (expanded[2]==0 || expanded[2]==' ')) {
        int id = 1; char *a = expanded+2;
        while (*a==' ') a++; if (*a=='%') a++; if (*a) id = atoi(a);
        builtin_bg(s, id); return 0;
    }
    if (!strncmp(expanded, "source ", 7)) { run_script(s, expanded+7, NULL, 0); return 0; }
    if (!strncmp(expanded, "bash ", 5))   { run_script(s, expanded+5, NULL, 0); return 0; }
    if (!strncmp(expanded, "multiWatch", 10)) { run_multiwatch(s, expanded); return 0; }

    if (strchr(expanded, '|')) run_pipeline(s, expanded);
    else {
        int bg = 0, L = strlen(expanded);
        if (L && expanded[L-1] == '&') { expanded[L-1] = 0; bg = 1; }
        run_simple(s, expanded, bg);
    }
    return 0;
}

/* ======================================================================
 * Worker — identical for pthread and forked child
 * ====================================================================== */
tab_worker(void *arg)
{
    Tab *s = (Tab *)arg;
    char line[MAX_LINE];

    while (1) {
        sem_wait(&s->wake);
        sem_wait(&s->lock);
        if (!s->alive) { sem_post(&s->lock); break; }
        if (s->qcount == 0) { sem_post(&s->lock); continue; }
        strcpy(line, s->queue[s->qhead]);
        s->qhead = (s->qhead + 1) % QSIZE;
        s->qcount--;
        sem_post(&s->lock);

        exec_line(s, line);
    }
    return NULL;
}

/* ======================================================================
 * GUI
 * ====================================================================== */
focus_window()
{
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, win, &a)) return;
    if (a.map_state == IsViewable) XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
}

redraw()
{
    Tab *s = &S[cur_tab];
    char copy[SCR_BUF], cur[MAX_LINE+64], hdr[128];
    char *lines[2000];
    int nlines = 0, i, start, end, y;

    XClearWindow(dpy, win);
    hdr[0] = 0;
    for (i = 0; i < MAX_TABS; i++) {
        if (!S[i].alive) continue;
        { char t[16]; snprintf(t, sizeof t, i==cur_tab?"[*%d] ":"[%d] ", i+1); strcat(hdr, t); }
    }
    XDrawString(dpy, win, gc, 10, 15, hdr, strlen(hdr));

    sem_wait(&s->lock);
    strcpy(copy, s->screen);
    sem_post(&s->lock);

    lines[nlines++] = copy;
    for (i = 0; copy[i]; i++) if (copy[i]=='\n') { copy[i] = 0; if (copy[i+1]) lines[nlines++] = &copy[i+1]; }
    if (s->scroll > nlines - VISIBLE) s->scroll = nlines - VISIBLE;
    if (s->scroll < 0) s->scroll = 0;
    end = nlines - s->scroll;
    start = end - VISIBLE; if (start < 0) start = 0;

    y = 40;
    for (i = start; i < end; i++) { XDrawString(dpy, win, gc, 10, y, lines[i], strlen(lines[i])); y += 18; }

    if (s->search_mode)    snprintf(cur, sizeof cur, "Enter search term: %s", s->search);
    else if (s->multiline) snprintf(cur, sizeof cur, "> %s", s->input);
    else                   snprintf(cur, sizeof cur, "$ %s", s->input);
    XDrawString(dpy, win, gc, 10, y, cur, strlen(cur));
    XFlush(dpy);
}

/* longest-common-substring length */
static int lcs_len(char *a, char *b)
{
    int best = 0, i, j;
    for (i = 0; a[i]; i++)
        for (j = 0; b[j]; j++) {
            int k = 0;
            while (a[i+k] && b[j+k] && a[i+k]==b[j+k]) k++;
            if (k > best) best = k;
        }
        return best;
}

run_search(Tab *s)
{
    int i, n, best = -1, bestlen = 0;
    char buf[MAX_LINE];

    sem_wait(&H->lock); n = H->count; sem_post(&H->lock);

    /* 1. most recent exact (substring) match */
    for (i = n-1; i >= 0; i--) {
        history_get(i, buf);
        if (strstr(buf, s->search)) { best = i; goto found; }
    }
    /* 2. longest common substring > 2 */
    for (i = 0; i < n; i++) {
        int l;
        history_get(i, buf);
        l = lcs_len(buf, s->search);
        if (l > bestlen && l > 2) { bestlen = l; best = i; }
    }
    if (best < 0) { scr_add(s, "No match for search term in history\n"); return; }

    found:
    history_get(best, buf);
    scr_add(s, buf); scr_add(s, "\n");
    strcpy(s->input, buf); s->inlen = strlen(buf); s->cursor = s->inlen;
}

complete(Tab *s)
{
    int start = s->cursor, len, n = 0, i, j;
    char prefix[256], names[64][256], lcp[256], t2[256];
    struct dirent *de; DIR *d;

    while (start > 0 && !isspace((unsigned char)s->input[start-1])) start--;
    len = s->cursor - start;
    if (len >= 256) return;
    memcpy(prefix, s->input+start, len); prefix[len] = 0;

    d = opendir(s->cwd); if (!d) return;
    while ((de = readdir(d)) && n < 64) {
        if (de->d_name[0]=='.' && prefix[0]!='.') continue;
        if (prefix[0] && strncmp(de->d_name, prefix, strlen(prefix)) != 0) continue;
        strncpy(names[n], de->d_name, 255); names[n][255] = 0; n++;
    }
    closedir(d);
    if (n == 0) return;

    for (i = 0; i < n; i++)
        for (j = i+1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                strcpy(t2, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t2);
            }

            if (n == 1) {
                int nl = strlen(names[0]);
                memmove(s->input+start+nl, s->input+start+len, strlen(s->input+start+len)+1);
                memcpy(s->input+start, names[0], nl);
                s->cursor = start + nl; s->inlen = strlen(s->input);
                return;
            }
            strcpy(lcp, names[0]);
            for (i = 1; i < n; i++) { int l = 0; while (lcp[l] && lcp[l]==names[i][l]) l++; lcp[l] = 0; }
            if ((int)strlen(lcp) > len) {
                int nl = strlen(lcp);
                memmove(s->input+start+nl, s->input+start+len, strlen(s->input+start+len)+1);
                memcpy(s->input+start, lcp, nl);
                s->cursor = start + nl; s->inlen = strlen(s->input);
                return;
            }
            scr_add(s, "\n");
            for (i = 0; i < n; i++) { char m[300]; snprintf(m, sizeof m, "%d. %s\n", i+1, names[i]); scr_add(s, m); }
}

handle_key(Tab *s, KeySym key, char *text, int len, unsigned state)
{
    char *act;

    if (s->search_mode) {
        if (key == XK_Return) { s->search_mode = 0; run_search(s); s->search_len = 0; s->search[0] = 0; }
        else if (key == XK_BackSpace) { if (s->search_len) s->search[--s->search_len] = 0; }
        else if (len > 0 && s->search_len < MAX_LINE-1) { s->search[s->search_len++] = text[0]; s->search[s->search_len] = 0; }
        return;
    }

    if (key == XK_Return) {
        s->input[s->inlen] = 0;
        if (needs_more(s->input)) {
            if (s->inlen > 0 && s->input[s->inlen-1] == '\\') {
                s->input[s->inlen-1] = 0; s->inlen--;
                strncat(s->multi, s->input, sizeof s->multi - strlen(s->multi) - 1);
            } else {
                strncat(s->multi, s->input, sizeof s->multi - strlen(s->multi) - 1);
                strcat(s->multi, "\n");
            }
            s->multiline = 1; s->inlen = 0; s->cursor = 0; s->input[0] = 0;
            return;
        }
        {
            char full[MAX_LINE*2], echo[MAX_LINE*2+8];
            strcpy(full, s->multi);
            strncat(full, s->input, sizeof full - strlen(full) - 1);
            s->multi[0] = 0; s->multiline = 0;
            s->inlen = 0; s->cursor = 0; s->input[0] = 0;
            if (*full) {
                snprintf(echo, sizeof echo, "$ %s\n", full);
                scr_add(s, echo);
                sem_wait(&s->lock);
                if (s->qcount < QSIZE) {
                    strncpy(s->queue[s->qtail], full, MAX_LINE-1);
                    s->queue[s->qtail][MAX_LINE-1] = 0;
                    s->qtail = (s->qtail + 1) % QSIZE;
                    s->qcount++;
                    sem_post(&s->wake);
                }
                sem_post(&s->lock);
            }
        }
        return;
    }

    if (key == XK_BackSpace) {
        if (s->cursor > 0) {
            memmove(s->input+s->cursor-1, s->input+s->cursor, s->inlen-s->cursor+1);
            s->cursor--; s->inlen--;
        }
        return;
    }
    if (key == XK_Left)  { if (s->cursor > 0) s->cursor--; return; }
    if (key == XK_Right) { if (s->cursor < s->inlen) s->cursor++; return; }
    if (key == XK_Page_Up)   { s->scroll += 10; return; }
    if (key == XK_Page_Down) { s->scroll -= 10; if (s->scroll < 0) s->scroll = 0; return; }
    if (key == XK_Tab) { complete(s); return; }

    if ((state & ControlMask) && key >= XK_1 && key <= XK_8) {
        int t = key - XK_1;
        if (t < MAX_TABS && S[t].alive) cur_tab = t;
        return;
    }
    if ((state & ControlMask) && key == XK_t) {
        int i;
        for (i = 0; i < MAX_TABS; i++) if (!S[i].alive) {
            sem_t l, w;
            sem_init(&l, 1, 1); sem_init(&w, 1, 0);
            sem_wait(&S[i].lock); /* not held yet, init directly instead */
            sem_post(&S[i].lock);
            /* reinitialize cleanly (we hold no state yet) */
            sem_destroy(&S[i].lock); sem_destroy(&S[i].wake);
            sem_init(&S[i].lock, 1, 1);
            sem_init(&S[i].wake, 1, 0);
            S[i].alive = 1;
            sem_wait(&S[cur_tab].lock);
            strcpy(S[i].cwd, S[cur_tab].cwd);
            sem_post(&S[cur_tab].lock);
            S[i].qhead = S[i].qtail = S[i].qcount = 0;
            S[i].screen[0] = 0;
            S[i].status = 0;
            S[i].fg_pid = 0;
            S[i].mw_active = 0;
            S[i].input[0] = 0; S[i].inlen = 0; S[i].cursor = 0;
            S[i].multiline = 0; S[i].multi[0] = 0;
            S[i].search_mode = 0; S[i].search_len = 0; S[i].search[0] = 0;
            S[i].scroll = 0; S[i].njobs = 0;
            cur_tab = i;
            if (use_process) {
                pid_t p = fork();
                if (p == 0) { tab_worker(&S[i]); _exit(0); }
            } else pthread_create(&threads[i], NULL, tab_worker, &S[i]);
            return;
        }
        return;
    }
    if ((state & ControlMask) && key == XK_w) {
        int i;
        sem_wait(&s->lock); s->alive = 0; sem_post(&s->lock);
        sem_post(&s->wake);
        for (i = 0; i < MAX_TABS; i++) if (S[i].alive) { cur_tab = i; return; }
        running = 0;
        return;
    }
    if ((state & ControlMask) && key == XK_c) {
        if (s->mw_active) { s->mw_active = 0; return; }
        if (s->fg_pid > 0) kill(s->fg_pid, SIGINT);
        s->inlen = 0; s->cursor = 0; s->input[0] = 0;
        return;
    }
    if ((state & ControlMask) && key == XK_z) {
        if (s->fg_pid > 0) kill(s->fg_pid, SIGTSTP);
        return;
    }

    act = bind_for(key, state);
    if (act) {
        if (!strcmp(act, "clear")) { scr_clear(s); return; }
        if (!strcmp(act, "exit"))  { running = 0; return; }
        if (!strcmp(act, "search")) { s->search_mode = 1; s->search_len = 0; s->search[0] = 0; return; }
        if (!strcmp(act, "home")) { s->cursor = 0; return; }
        if (!strcmp(act, "end"))  { s->cursor = s->inlen; return; }
        if (!strcmp(act, "kill_word")) {
            int p = s->cursor;
            while (p > 0 && isspace((unsigned char)s->input[p-1])) p--;
            while (p > 0 && !isspace((unsigned char)s->input[p-1])) p--;
            memmove(s->input+p, s->input+s->cursor, s->inlen-s->cursor+1);
            s->inlen -= s->cursor - p; s->cursor = p;
            return;
        }
        if (!strcmp(act, "up")) {
            int i, n; char buf[MAX_LINE];
            sem_wait(&H->lock); n = H->count; sem_post(&H->lock);
            for (i = n-1; i >= 0; i--) {
                history_get(i, buf);
                if (strcmp(buf, s->input) != 0) {
                    strcpy(s->input, buf); s->inlen = strlen(buf); s->cursor = s->inlen;
                    return;
                }
            }
            return;
        }
        if (!strcmp(act, "down")) { s->input[0] = 0; s->inlen = 0; s->cursor = 0; return; }
    }

    if (len > 0 && s->inlen < MAX_LINE-2) {
        memmove(s->input+s->cursor+1, s->input+s->cursor, s->inlen-s->cursor+1);
        s->input[s->cursor] = text[0]; s->cursor++; s->inlen++;
    }
}

init_ui()
{
    int i;
    S = mmap(NULL, sizeof(Tab) * MAX_TABS, PROT_READ|PROT_WRITE,
             MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    memset(S, 0, sizeof(Tab) * MAX_TABS);
    for (i = 0; i < MAX_TABS; i++) {
        sem_init(&S[i].lock, 1, 1);
        sem_init(&S[i].wake, 1, 0);
    }
    S[0].alive = 1;
    getcwd(S[0].cwd, sizeof S[0].cwd);

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open display\n"); exit(1); }
    scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 10, 10, 900, 600, 1,
                              BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XSelectInput(dpy, win, ExposureMask|KeyPressMask|ButtonPressMask|FocusChangeMask|StructureNotifyMask);
    XMapWindow(dpy, win);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, scr));
    XSetBackground(dpy, gc, WhitePixel(dpy, scr));

    signal(SIGTSTP, SIG_IGN);   /* Terminator itself must never stop */

    if (use_process) {
        pid_t p = fork();
        if (p == 0) { tab_worker(&S[0]); _exit(0); }
    } else pthread_create(&threads[0], NULL, tab_worker, &S[0]);
}

main(int argc, char **argv)
{
    int i;
    XEvent event; KeySym key; char text[32]; int len;

    for (i = 1; i < argc; i++)
        if (!strncmp(argv[i], "useProcess", 10)) use_process = 1;

        history_init();
    load_binds();
    init_ui();

    while (running) {
        fd_set fds; struct timeval tv;
        FD_ZERO(&fds); FD_SET(ConnectionNumber(dpy), &fds);
        tv.tv_sec = 0; tv.tv_usec = 20000;
        select(ConnectionNumber(dpy)+1, &fds, NULL, NULL, &tv);

        while (XPending(dpy)) {
            XNextEvent(dpy, &event);
            if (event.type == Expose) redraw();
            else if (event.type == MapNotify) focus_window();
            else if (event.type == ButtonPress) {
                focus_window();
                if (event.xbutton.button == Button4) { S[cur_tab].scroll += 3; redraw(); }
                else if (event.xbutton.button == Button5) { S[cur_tab].scroll -= 3; if (S[cur_tab].scroll < 0) S[cur_tab].scroll = 0; redraw(); }
            } else if (event.type == KeyPress) {
                key = NoSymbol;
                len = XLookupString(&event.xkey, text, sizeof text, &key, NULL);
                if (key == NoSymbol && len == 0) continue;
                handle_key(&S[cur_tab], key, text, len, event.xkey.state);
                redraw();
            }
        }
        /* refresh every 20ms so worker output appears */
        redraw();
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
