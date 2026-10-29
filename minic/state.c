/* state.c - 컨테이너 상태를 파일로 저장하고 읽는다
 *
 * /var/run/minic/<name>/info   key=value 형식의 설정
 * /var/run/minic/<name>/log    백그라운드 실행 시 출력 로그
 * /var/run/minic/<name>/exit   종료 결과 ("exit 0" 또는 "signal 9")
 */
#include "minic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

void state_path(char *buf, size_t n, const char *name, const char *file)
{
    if (file)
        snprintf(buf, n, "%s/%s/%s", STATE_DIR, name, file);
    else
        snprintf(buf, n, "%s/%s", STATE_DIR, name);
}

/* 새 컨테이너용 상태 디렉터리를 깨끗하게 만든다 */
int state_prepare(const char *name)
{
    char dir[PATH_MAX];
    if (mkdir(STATE_DIR, 0755) < 0 && errno != EEXIST)
        return -1;
    state_remove(name);
    state_path(dir, sizeof dir, name, NULL);
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int state_save(const struct cinfo *c)
{
    char path[PATH_MAX], buf[PATH_MAX + 1024];
    snprintf(buf, sizeof buf,
             "name=%s\npid=%d\nstart=%ld\nmem=%lld\ncpu=%d\npids=%d\n"
             "detached=%d\nrootfs=%s\ncmd=%s\n",
             c->name, c->pid, (long)c->start, c->mem_limit, c->cpu_percent,
             c->pids_limit, c->detached, c->rootfs, c->cmd);
    state_path(path, sizeof path, c->name, "info");
    return write_str(path, buf, 1);
}

int state_load(const char *name, struct cinfo *c)
{
    static char buf[PATH_MAX + 1024];
    char path[PATH_MAX];

    state_path(path, sizeof path, name, "info");
    if (read_str(path, buf, sizeof buf) < 0)
        return -1;

    memset(c, 0, sizeof *c);
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "name"))          snprintf(c->name, sizeof c->name, "%s", v);
        else if (!strcmp(k, "pid"))      c->pid = atoi(v);
        else if (!strcmp(k, "start"))    c->start = atol(v);
        else if (!strcmp(k, "mem"))      c->mem_limit = atoll(v);
        else if (!strcmp(k, "cpu"))      c->cpu_percent = atoi(v);
        else if (!strcmp(k, "pids"))     c->pids_limit = atoi(v);
        else if (!strcmp(k, "detached")) c->detached = atoi(v);
        else if (!strcmp(k, "rootfs"))   snprintf(c->rootfs, sizeof c->rootfs, "%s", v);
        else if (!strcmp(k, "cmd"))      snprintf(c->cmd, sizeof c->cmd, "%s", v);
    }
    return c->name[0] ? 0 : -1;
}

void state_remove(const char *name)
{
    static const char *files[] = { "info", "log", "exit" };
    char p[PATH_MAX];
    for (int i = 0; i < 3; i++) {
        state_path(p, sizeof p, name, files[i]);
        unlink(p);
    }
    state_path(p, sizeof p, name, NULL);
    rmdir(p);
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* 저장된 컨테이너 이름 목록 (이름순). 개수를 반환한다 */
int state_list(char names[][64], int max)
{
    DIR *d = opendir(STATE_DIR);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= 64)
            continue;
        snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    qsort(names, n, 64, cmp_name);
    return n;
}

void state_write_exit(const char *name, int wstatus)
{
    char p[PATH_MAX], v[32];
    if (WIFEXITED(wstatus))
        snprintf(v, sizeof v, "exit %d", WEXITSTATUS(wstatus));
    else
        snprintf(v, sizeof v, "signal %d", WTERMSIG(wstatus));
    state_path(p, sizeof p, name, "exit");
    write_str(p, v, 1);
}

int container_alive(const struct cinfo *c)
{
    return proc_alive(c->pid);
}

void container_status(const struct cinfo *c, char *out, size_t n)
{
    char p[PATH_MAX], buf[64];
    int code;

    if (container_alive(c)) {
        snprintf(out, n, "running");
        return;
    }
    state_path(p, sizeof p, c->name, "exit");
    if (read_str(p, buf, sizeof buf) > 0) {
        if (sscanf(buf, "exit %d", &code) == 1)
            snprintf(out, n, "exited(%d)", code);
        else if (sscanf(buf, "signal %d", &code) == 1)
            snprintf(out, n, "killed(%d)", code);
        else
            snprintf(out, n, "exited");
    } else {
        snprintf(out, n, "dead");
    }
}
