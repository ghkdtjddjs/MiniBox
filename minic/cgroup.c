/* cgroup.c - 자원 제한과 사용량 측정
 *
 * Ubuntu 24.04는 cgroup v2(통합 계층)를 쓴다.
 *   /sys/fs/cgroup/minic/<name>/{memory.max, cpu.max, pids.max, cgroup.procs ...}
 * 오래된 시스템(v1)은 컨트롤러마다 디렉터리가 따로 있어서 자동으로 대체한다.
 *   /sys/fs/cgroup/<memory|cpu|cpuacct|pids>/minic/<name>/...
 */
#include "minic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define NV1 4
static const char *V1_CTL[NV1] = { "memory", "cpu", "cpuacct", "pids" };

static int use_v2(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (access(CG_ROOT "/cgroup.controllers", F_OK) == 0);
    return cached;
}

const char *cg_version(void)
{
    return use_v2() ? "cgroup v2" : "cgroup v1";
}

/* cgroup 경로 만들기. name이나 file이 NULL이면 거기까지만 만든다 */
static void cg_path(char *buf, size_t n, const char *ctl, const char *name, const char *file)
{
    int off;
    if (use_v2())
        off = snprintf(buf, n, "%s/%s", CG_ROOT, CG_PARENT);
    else
        off = snprintf(buf, n, "%s/%s/%s", CG_ROOT, ctl, CG_PARENT);
    if (name && off < (int)n)
        off += snprintf(buf + off, n - off, "/%s", name);
    if (file && off < (int)n)
        snprintf(buf + off, n - off, "/%s", file);
}

static void make_dir(const char *p)
{
    if (mkdir(p, 0755) < 0 && errno != EEXIST)
        fprintf(stderr, "minic: mkdir %s: %s\n", p, strerror(errno));
}

static void set_limit(const char *path, const char *value, const char *what)
{
    if (write_str(path, value, 0) < 0)
        fprintf(stderr, "minic: 경고: %s 제한을 설정하지 못했습니다 (%s: %s)\n",
                what, path, strerror(errno));
}

/* 컨테이너용 cgroup을 만들고 제한값을 설정한다. 성공하면 0 */
int cg_create(const char *name, long long mem, int cpu, int pids)
{
    char p[PATH_MAX], v[64];

    if (use_v2()) {
        /* 상위 그룹에서 하위 그룹이 쓸 컨트롤러를 켠다 (이미 켜져 있으면 무시) */
        static const char *want[] = { "+memory", "+cpu", "+pids" };
        cg_path(p, sizeof p, NULL, NULL, NULL);
        make_dir(p);
        for (int i = 0; i < 3; i++)
            write_str(CG_ROOT "/cgroup.subtree_control", want[i], 0);
        cg_path(p, sizeof p, NULL, NULL, "cgroup.subtree_control");
        for (int i = 0; i < 3; i++)
            write_str(p, want[i], 0);

        cg_path(p, sizeof p, NULL, name, NULL);
        make_dir(p);
        if (access(p, F_OK) < 0)
            return -1;

        if (mem > 0) {
            snprintf(v, sizeof v, "%lld", mem);
            cg_path(p, sizeof p, NULL, name, "memory.max");
            set_limit(p, v, "메모리");
            cg_path(p, sizeof p, NULL, name, "memory.swap.max");
            write_str(p, "0", 0);            /* swap으로 한도를 피하지 못하게 */
        }
        if (cpu > 0) {
            snprintf(v, sizeof v, "%d 100000", cpu * 1000);
            cg_path(p, sizeof p, NULL, name, "cpu.max");
            set_limit(p, v, "CPU");
        }
        if (pids > 0) {
            snprintf(v, sizeof v, "%d", pids);
            cg_path(p, sizeof p, NULL, name, "pids.max");
            set_limit(p, v, "프로세스 수");
        }
        return 0;
    }

    /* v1: 컨트롤러별 디렉터리에 같은 이름의 그룹을 만든다 */
    int made = 0;
    for (int i = 0; i < NV1; i++) {
        char top[PATH_MAX];
        snprintf(top, sizeof top, "%s/%s", CG_ROOT, V1_CTL[i]);
        if (access(top, F_OK) < 0)
            continue;
        cg_path(p, sizeof p, V1_CTL[i], NULL, NULL);
        make_dir(p);
        cg_path(p, sizeof p, V1_CTL[i], name, NULL);
        make_dir(p);
        if (access(p, F_OK) == 0)
            made++;
    }
    if (!made)
        return -1;

    if (mem > 0) {
        snprintf(v, sizeof v, "%lld", mem);
        cg_path(p, sizeof p, "memory", name, "memory.limit_in_bytes");
        set_limit(p, v, "메모리");
        cg_path(p, sizeof p, "memory", name, "memory.memsw.limit_in_bytes");
        write_str(p, v, 0);                  /* swap 포함 한도 (있을 때만) */
    }
    if (cpu > 0) {
        cg_path(p, sizeof p, "cpu", name, "cpu.cfs_period_us");
        write_str(p, "100000", 0);
        snprintf(v, sizeof v, "%d", cpu * 1000);
        cg_path(p, sizeof p, "cpu", name, "cpu.cfs_quota_us");
        set_limit(p, v, "CPU");
    }
    if (pids > 0) {
        snprintf(v, sizeof v, "%d", pids);
        cg_path(p, sizeof p, "pids", name, "pids.max");
        set_limit(p, v, "프로세스 수");
    }
    return 0;
}

/* 프로세스를 cgroup에 넣는다. 이후 이 프로세스와 자식들이 제한을 받는다 */
int cg_add_pid(const char *name, pid_t pid)
{
    char p[PATH_MAX], v[32];
    int ok = 0;
    snprintf(v, sizeof v, "%d", pid);

    if (use_v2()) {
        cg_path(p, sizeof p, NULL, name, "cgroup.procs");
        return write_str(p, v, 0);
    }
    for (int i = 0; i < NV1; i++) {
        cg_path(p, sizeof p, V1_CTL[i], name, "cgroup.procs");
        if (write_str(p, v, 0) == 0)
            ok++;
    }
    return ok ? 0 : -1;
}

long long cg_mem_current(const char *name)
{
    char p[PATH_MAX];
    if (use_v2())
        cg_path(p, sizeof p, NULL, name, "memory.current");
    else
        cg_path(p, sizeof p, "memory", name, "memory.usage_in_bytes");
    return read_ll(p);
}

/* 누적 CPU 사용 시간(마이크로초). 두 번 재서 차이로 사용률을 구한다 */
long long cg_cpu_usec(const char *name)
{
    char p[PATH_MAX], buf[1024];
    if (use_v2()) {
        cg_path(p, sizeof p, NULL, name, "cpu.stat");
        if (read_str(p, buf, sizeof buf) < 0)
            return -1;
        char *s = strstr(buf, "usage_usec");
        return s ? strtoll(s + 10, NULL, 10) : -1;
    }
    const char *try_ctl[2] = { "cpuacct", "cpu" };
    for (int i = 0; i < 2; i++) {
        cg_path(p, sizeof p, try_ctl[i], name, "cpuacct.usage");
        long long ns = read_ll(p);
        if (ns >= 0)
            return ns / 1000;                /* v1은 나노초 단위 */
    }
    return -1;
}

long long cg_pids_current(const char *name)
{
    char p[PATH_MAX];
    cg_path(p, sizeof p, "pids", name, "pids.current");
    return read_ll(p);
}

/* 그룹 삭제. 프로세스가 완전히 사라질 때까지 잠깐 재시도한다 */
void cg_destroy(const char *name)
{
    char p[PATH_MAX];
    for (int tries = 0; tries < 30; tries++) {
        int left = 0;
        if (use_v2()) {
            cg_path(p, sizeof p, NULL, name, NULL);
            if (rmdir(p) < 0 && errno != ENOENT)
                left++;
        } else {
            for (int i = 0; i < NV1; i++) {
                cg_path(p, sizeof p, V1_CTL[i], name, NULL);
                if (rmdir(p) < 0 && errno != ENOENT)
                    left++;
            }
        }
        if (!left)
            return;
        msleep(100);
    }
}
