/* enter.c - 이미 실행 중인 컨테이너 안으로 들어가기 (minic exec)
 *
 * /proc/<pid>/ns/ 아래 파일을 열어서 setns()로 같은 namespace에 합류하고,
 * /proc/<pid>/root 로 chroot해서 같은 파일시스템을 보게 만든다.
 * PID namespace는 setns() 이후에 만든 "자식"부터 적용되므로 fork를 한 번 더 한다.
 */
#include "minic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <sys/wait.h>

#define NNS 4
static const char *NS_NAMES[NNS] = { "pid", "ipc", "uts", "mnt" };

/* 컨테이너 안에서 fn(arg)를 실행하는 프로세스를 만든다. 반환값은 중간 프로세스 PID */
pid_t spawn_in_container(const struct cinfo *ci, int (*fn)(void *), void *arg)
{
    fflush(stdout);
    pid_t p = fork();
    if (p != 0)
        return p;                             /* 부모(또는 실패 시 -1) */

    int fds[NNS];
    char path[64];

    /* 1) namespace 파일을 먼저 전부 연다 (mnt에 들어가면 호스트 /proc이 안 보이므로) */
    for (int i = 0; i < NNS; i++) {
        snprintf(path, sizeof path, "/proc/%d/ns/%s", ci->pid, NS_NAMES[i]);
        fds[i] = open(path, O_RDONLY | O_CLOEXEC);
        if (fds[i] < 0) {
            fprintf(stderr, "minic: %s: %s\n", path, strerror(errno));
            _exit(126);
        }
    }
    snprintf(path, sizeof path, "/proc/%d/root", ci->pid);
    int rootfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) {
        fprintf(stderr, "minic: %s: %s\n", path, strerror(errno));
        _exit(126);
    }

    /* 2) cgroup 합류 (chroot 전에 해야 /sys/fs/cgroup 이 보인다) */
    cg_add_pid(ci->name, getpid());

    /* 3) namespace 합류 */
    for (int i = 0; i < NNS; i++) {
        if (setns(fds[i], 0) < 0) {
            fprintf(stderr, "minic: setns(%s): %s\n", NS_NAMES[i], strerror(errno));
            _exit(126);
        }
        close(fds[i]);
    }

    /* 4) 컨테이너의 루트로 이동 */
    if (fchdir(rootfd) < 0 || chroot(".") < 0 || chdir("/") < 0) {
        perror("minic: chroot");
        _exit(126);
    }
    close(rootfd);

    /* 5) 새 PID namespace는 자식부터 적용되므로 한 번 더 fork */
    pid_t g = fork();
    if (g < 0)
        _exit(126);
    if (g == 0)
        _exit(fn(arg));

    signal(SIGINT, SIG_IGN);
    _exit(wait_code(g));
}

/* waitpid 결과를 셸처럼 숫자 하나로: 정상 종료는 종료 코드, 시그널은 128+번호 */
int wait_code(pid_t pid)
{
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return 128 + WTERMSIG(st);
}

static int exec_fn(void *arg)
{
    char **argv = arg;
    const char *term = getenv("TERM");
    char termbuf[64] = "xterm";
    if (term)
        snprintf(termbuf, sizeof termbuf, "%s", term);
    clearenv();
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/", 1);
    setenv("TERM", termbuf, 1);
    tty_take();
    execvp(argv[0], argv);
    fprintf(stderr, "minic: '%s' 실행 실패: %s\n", argv[0], strerror(errno));
    return 127;
}

int cmd_exec(int argc, char **argv)
{
    static char *default_cmd[] = { "/bin/sh", NULL };
    struct cinfo ci;

    if (argc < 2) {
        fprintf(stderr, "사용법: minic exec <name> [명령 [인자...]]\n");
        return 1;
    }
    if (state_load(argv[1], &ci) < 0)
        die("'%s' 컨테이너를 찾을 수 없습니다", argv[1]);
    if (!container_alive(&ci))
        die("'%s' 는 실행 중이 아닙니다", argv[1]);

    char **cmdv = (argc > 2) ? &argv[2] : default_cmd;
    pid_t p = spawn_in_container(&ci, exec_fn, cmdv);
    if (p < 0)
        die("fork 실패: %s", strerror(errno));

    signal(SIGINT, SIG_IGN);              /* Ctrl-C는 컨테이너 안 프로그램에게 */
    int code = wait_code(p);
    tty_restore();
    return code < 0 ? 1 : code;
}
