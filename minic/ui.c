/* ui.c - 사용자에게 보여주는 화면들
 *   ps       : 컨테이너 목록 표
 *   stats    : CPU/메모리/프로세스 수 실시간 모니터 (curses)
 *   compare  : 호스트와 컨테이너가 보는 세상을 나란히 비교 (격리 효과 확인)
 */
#include "minic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <ncurses.h>

/* ------------------------------------------------------------------ */
/* 공통 도구                                                           */
/* ------------------------------------------------------------------ */

static long long host_mem_total(void)
{
    char buf[256];
    long long kb = 0;
    if (read_str("/proc/meminfo", buf, sizeof buf) < 0)
        return -1;
    if (sscanf(buf, "MemTotal: %lld kB", &kb) != 1)
        return -1;
    return kb * 1024;
}

/* [#####-----] 형태의 막대 */
static void make_bar(char *out, double frac, int width)
{
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    int fill = (int)(frac * width + 0.5);
    out[0] = '[';
    for (int i = 0; i < width; i++)
        out[1 + i] = (i < fill) ? '#' : '-';
    out[width + 1] = ']';
    out[width + 2] = '\0';
}

/* 이전 측정값을 기억해서 CPU 사용률(%)을 계산한다. 첫 측정이면 -1 */
struct prev_cpu {
    char      name[64];
    long long usec;
    long long t;
};
static struct prev_cpu prevs[MAX_CONT];
static int nprev;

static double cpu_percent(const char *name)
{
    long long u = cg_cpu_usec(name), t = now_usec();
    for (int i = 0; i < nprev; i++) {
        if (strcmp(prevs[i].name, name) != 0)
            continue;
        double pct = -1;
        if (u >= 0 && prevs[i].usec >= 0 && t > prevs[i].t)
            pct = (u - prevs[i].usec) * 100.0 / (double)(t - prevs[i].t);
        prevs[i].usec = u;
        prevs[i].t = t;
        return pct;
    }
    if (nprev < MAX_CONT) {
        snprintf(prevs[nprev].name, sizeof prevs[nprev].name, "%s", name);
        prevs[nprev].usec = u;
        prevs[nprev].t = t;
        nprev++;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* ps                                                                   */
/* ------------------------------------------------------------------ */

int cmd_ps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char names[MAX_CONT][64];
    int n = state_list(names, MAX_CONT);

    printf("%-14s %-8s %-11s %-8s %-21s %s\n",
           "NAME", "PID", "STATUS", "UPTIME", "MEM(USED/LIMIT)", "COMMAND");
    if (n == 0) {
        printf("(컨테이너가 없습니다)\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        struct cinfo ci;
        char status[32], up[16] = "-", used[16] = "-", lim[16] = "-", mem[40];
        if (state_load(names[i], &ci) < 0)
            continue;
        container_status(&ci, status, sizeof status);
        if (container_alive(&ci)) {
            fmt_uptime(ci.start, up, sizeof up);
            fmt_bytes(cg_mem_current(ci.name), used, sizeof used);
        }
        if (ci.mem_limit > 0)
            fmt_bytes(ci.mem_limit, lim, sizeof lim);
        snprintf(mem, sizeof mem, "%s/%s", used, lim);
        printf("%-14s %-8d %-11s %-8s %-21s %.40s\n",
               ci.name, ci.pid, status, up, mem, ci.cmd);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* stats                                                                */
/* ------------------------------------------------------------------ */

struct row {
    struct cinfo ci;
    double       cpu;      /* % (모르면 -1) */
    long long    mem;
    long long    pids;
};

/* 실행 중인 컨테이너들의 현재 값을 모은다 */
static int collect_rows(struct row *rows, int max)
{
    char names[MAX_CONT][64];
    int n = state_list(names, MAX_CONT), cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        struct row *r = &rows[cnt];
        if (state_load(names[i], &r->ci) < 0 || !container_alive(&r->ci))
            continue;
        r->cpu = cpu_percent(r->ci.name);
        r->mem = cg_mem_current(r->ci.name);
        r->pids = cg_pids_current(r->ci.name);
        cnt++;
    }
    return cnt;
}

static void format_row(const struct row *r, long long memtotal, char *out, size_t n)
{
    char cbar[16], mbar[16], used[16], lim[16], cpu[24], pids[24];
    int cpu_cap = r->ci.cpu_percent > 0 ? r->ci.cpu_percent : 100;
    long long mem_cap = r->ci.mem_limit > 0 ? r->ci.mem_limit : memtotal;

    make_bar(cbar, r->cpu < 0 ? 0 : r->cpu / cpu_cap, 10);
    make_bar(mbar, mem_cap > 0 && r->mem > 0 ? (double)r->mem / mem_cap : 0, 10);
    fmt_bytes(r->mem, used, sizeof used);
    if (r->ci.mem_limit > 0)
        fmt_bytes(r->ci.mem_limit, lim, sizeof lim);
    else
        snprintf(lim, sizeof lim, "none");

    if (r->cpu < 0)
        snprintf(cpu, sizeof cpu, "   -  ");
    else
        snprintf(cpu, sizeof cpu, "%5.1f%%", r->cpu);
    if (r->ci.cpu_percent > 0)
        snprintf(cpu + strlen(cpu), sizeof cpu - strlen(cpu), "/%d%%", r->ci.cpu_percent);

    if (r->ci.pids_limit > 0)
        snprintf(pids, sizeof pids, "%lld/%d", r->pids, r->ci.pids_limit);
    else
        snprintf(pids, sizeof pids, "%lld", r->pids);

    snprintf(out, n, "%-12s %-7d %s %-12s %s %8s/%-8s %s",
             r->ci.name, r->ci.pid, cbar, cpu, mbar, used, lim, pids);
}

static const char *STATS_HEAD =
    "NAME         PID     CPU                       MEM                            PIDS";

static int stats_once(void)
{
    struct row rows[MAX_CONT];
    char line[256];
    long long memtotal = host_mem_total();

    collect_rows(rows, MAX_CONT);           /* 첫 측정 (기준값) */
    msleep(1000);
    int n = collect_rows(rows, MAX_CONT);   /* 1초 뒤 측정 -> 차이로 CPU% 계산 */

    printf("%s\n", STATS_HEAD);
    if (n == 0)
        printf("(실행 중인 컨테이너가 없습니다)\n");
    for (int i = 0; i < n; i++) {
        format_row(&rows[i], memtotal, line, sizeof line);
        printf("%s\n", line);
    }
    return 0;
}

int cmd_stats(int argc, char **argv)
{
    if ((argc > 1 && !strcmp(argv[1], "--once")) || !isatty(STDOUT_FILENO))
        return stats_once();

    struct row rows[MAX_CONT];
    char line[256], clock[16];
    long long memtotal = host_mem_total();

    initscr();
    noecho();
    cbreak();
    curs_set(0);
    timeout(1000);                           /* 1초마다 화면 갱신 */

    for (;;) {
        int n = collect_rows(rows, MAX_CONT);
        time_t now = time(NULL);
        strftime(clock, sizeof clock, "%H:%M:%S", localtime(&now));

        erase();
        mvprintw(0, 0, "minic stats   (q: quit)   %s   %s", cg_version(), clock);
        mvprintw(2, 0, "%s", STATS_HEAD);
        if (n == 0)
            mvprintw(3, 0, "(no running containers)");
        for (int i = 0; i < n && i + 3 < LINES; i++) {
            format_row(&rows[i], memtotal, line, sizeof line);
            mvprintw(3 + i, 0, "%s", line);
        }
        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 'Q')
            break;
    }
    endwin();
    return 0;
}

/* ------------------------------------------------------------------ */
/* compare: 호스트 vs 컨테이너                                          */
/* ------------------------------------------------------------------ */

struct probe {
    char hostname[64];
    int  pid;
    int  nproc;
    int  nmounts;
    int  nroot;
    char rootlist[96];
};

/* 지금 이 프로세스가 "보고 있는" 세상을 조사한다 */
static void probe_here(struct probe *p)
{
    DIR *d;
    struct dirent *e;
    char buf[4096];

    memset(p, 0, sizeof *p);
    gethostname(p->hostname, sizeof p->hostname - 1);
    p->pid = getpid();

    if ((d = opendir("/proc")) != NULL) {         /* 숫자 이름 = 보이는 프로세스 */
        while ((e = readdir(d)) != NULL)
            if (isdigit((unsigned char)e->d_name[0]))
                p->nproc++;
        closedir(d);
    }

    int fd = open("/proc/self/mounts", O_RDONLY);  /* 한 줄 = 마운트 하나 */
    if (fd >= 0) {
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0)
            for (ssize_t i = 0; i < n; i++)
                if (buf[i] == '\n')
                    p->nmounts++;
        close(fd);
    }

    if ((d = opendir("/")) != NULL) {              /* 루트 디렉터리 내용 */
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            p->nroot++;
            size_t used = strlen(p->rootlist);
            if (used + strlen(e->d_name) + 2 < sizeof p->rootlist)
                snprintf(p->rootlist + used, sizeof p->rootlist - used,
                         "%s%s", used ? " " : "", e->d_name);
        }
        closedir(d);
    }
}

static int probe_fn(void *arg)
{
    int fd = *(int *)arg;
    struct probe p;
    probe_here(&p);
    return write(fd, &p, sizeof p) == (ssize_t)sizeof p ? 0 : 1;
}

static void cell(char *out, size_t n, const char *s)
{
    if (strlen(s) < 30)
        snprintf(out, n, "%s", s);
    else
        snprintf(out, n, "%.26s...", s);
}

static void print_row(const char *label, const char *host, const char *cont)
{
    char h[40], c[40];
    cell(h, sizeof h, host);
    cell(c, sizeof c, cont);
    printf("  %-18s | %-30s | %s\n", label, h, c);
}

int compare_container(const char *name)
{
    struct cinfo ci;
    struct probe host, in;
    int pfd[2];
    char a[160], b[160];

    if (state_load(name, &ci) < 0 || !container_alive(&ci)) {
        fprintf(stderr, "minic: '%s' 는 실행 중이 아닙니다\n", name);
        return -1;
    }
    if (pipe(pfd) < 0)
        return -1;

    pid_t p = spawn_in_container(&ci, probe_fn, &pfd[1]);
    close(pfd[1]);
    if (p < 0) {
        close(pfd[0]);
        return -1;
    }
    size_t got = 0;
    while (got < sizeof in) {
        ssize_t n = read(pfd[0], (char *)&in + got, sizeof in - got);
        if (n <= 0)
            break;
        got += n;
    }
    close(pfd[0]);
    wait_code(p);
    if (got != sizeof in) {
        fprintf(stderr, "minic: 컨테이너 내부 조사에 실패했습니다\n");
        return -1;
    }
    probe_here(&host);

    snprintf(b, sizeof b, "CONTAINER '%s'", ci.name);
    printf("\n");
    print_row("", "HOST", b);
    printf("  -------------------+--------------------------------+------------------------------\n");
    print_row("hostname", host.hostname, in.hostname);

    snprintf(a, sizeof a, "%d", host.pid);
    snprintf(b, sizeof b, "%d", in.pid);
    print_row("my PID", a, b);

    snprintf(a, sizeof a, "%d", host.nproc);
    snprintf(b, sizeof b, "%d", in.nproc);
    print_row("visible processes", a, b);

    snprintf(a, sizeof a, "%d", host.nmounts);
    snprintf(b, sizeof b, "%d", in.nmounts);
    print_row("visible mounts", a, b);

    snprintf(a, sizeof a, "%d: %s", host.nroot, host.rootlist);
    snprintf(b, sizeof b, "%d: %s", in.nroot, in.rootlist);
    print_row("/ entries", a, b);

    char t[16];
    fmt_bytes(host_mem_total(), t, sizeof t);
    snprintf(a, sizeof a, "%s (RAM total)", t);
    if (ci.mem_limit > 0)
        fmt_bytes(ci.mem_limit, b, sizeof b);
    else
        snprintf(b, sizeof b, "none");
    print_row("memory limit", a, b);

    if (ci.cpu_percent > 0)
        snprintf(b, sizeof b, "%d%% of 1 core", ci.cpu_percent);
    else
        snprintf(b, sizeof b, "none");
    print_row("cpu limit", "none", b);

    if (ci.pids_limit > 0)
        snprintf(b, sizeof b, "%d", ci.pids_limit);
    else
        snprintf(b, sizeof b, "none");
    print_row("pids limit", "none", b);

    printf("\n  -> 같은 컴퓨터, 같은 커널인데 컨테이너 안에서는 세상이 다르게 보입니다.\n\n");
    return 0;
}

int cmd_compare(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "사용법: minic compare <name>\n");
        return 1;
    }
    return compare_container(argv[1]) == 0 ? 0 : 1;
}
