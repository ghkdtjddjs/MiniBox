/* minic.h - 미니 컨테이너 공통 헤더 */
#ifndef MINIC_H
#define MINIC_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <limits.h>
#include <time.h>

#define STATE_DIR   "/var/run/minic"   /* 컨테이너 상태 파일 저장 위치 */
#define CG_ROOT     "/sys/fs/cgroup"
#define CG_PARENT   "minic"            /* cgroup 상위 그룹 이름 */
#define MAX_CONT    64
#define STACK_SIZE  (1024 * 1024)

/* 실행 중이거나 종료된 컨테이너 한 개의 정보 (state 파일에 저장) */
struct cinfo {
    char      name[64];
    pid_t     pid;            /* 호스트 기준 컨테이너 init PID */
    time_t    start;
    long long mem_limit;      /* byte, 0이면 제한 없음 */
    int       cpu_percent;    /* CPU 1개 기준 %, 0이면 제한 없음 */
    int       pids_limit;     /* 0이면 제한 없음 */
    int       detached;
    char      rootfs[PATH_MAX];
    char      cmd[256];
};

/* run 명령에 필요한 설정 */
struct run_cfg {
    char       name[64];
    char       rootfs[PATH_MAX];
    char     **cmdv;          /* NULL로 끝나는 argv */
    long long  mem_limit;
    int        cpu_percent;
    int        pids_limit;
};

/* ---------- util.c ---------- */
void      die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
int       write_str(const char *path, const char *s, int create);
int       write_all(int fd, const void *buf, size_t n);
ssize_t   read_str(const char *path, char *buf, size_t size);
long long read_ll(const char *path);
long long parse_size(const char *s);
int       valid_name(const char *s);
void      fmt_bytes(long long b, char *out, size_t n);
void      fmt_uptime(time_t start, char *out, size_t n);
int       proc_alive(pid_t pid);
void      tty_take(void);
void      tty_restore(void);
long long now_usec(void);
void      msleep(int ms);

/* ---------- cgroup.c (v2 우선, v1 자동 대체) ---------- */
int       cg_create(const char *name, long long mem, int cpu, int pids);
int       cg_add_pid(const char *name, pid_t pid);
long long cg_mem_current(const char *name);
long long cg_cpu_usec(const char *name);
long long cg_pids_current(const char *name);
void      cg_destroy(const char *name);
const char *cg_version(void);

/* ---------- state.c ---------- */
int  state_prepare(const char *name);
int  state_save(const struct cinfo *c);
int  state_load(const char *name, struct cinfo *c);
void state_remove(const char *name);
int  state_list(char names[][64], int max);
void state_write_exit(const char *name, int wstatus);
void state_path(char *buf, size_t n, const char *name, const char *file);
int  container_alive(const struct cinfo *c);
void container_status(const struct cinfo *c, char *out, size_t n);

/* ---------- container.c ---------- */
int cmd_run(int argc, char **argv);
int cmd_stop(int argc, char **argv);
int cmd_rm(int argc, char **argv);
int cmd_logs(int argc, char **argv);
int container_start(const struct run_cfg *cfg, int detached, int *status);
int container_stop(const char *name, int timeout_sec);

/* ---------- enter.c ---------- */
pid_t spawn_in_container(const struct cinfo *ci, int (*fn)(void *), void *arg);
int   wait_code(pid_t pid);
int   cmd_exec(int argc, char **argv);

/* ---------- ui.c ---------- */
int cmd_ps(int argc, char **argv);
int cmd_stats(int argc, char **argv);
int cmd_compare(int argc, char **argv);
int compare_container(const char *name);

/* ---------- demo.c ---------- */
int cmd_demo(int argc, char **argv);

#endif
