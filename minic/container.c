/* container.c - 컨테이너 실행(run), 종료(stop), 삭제(rm), 로그(logs)
 *
 * 핵심 흐름
 *   부모: clone(NEWPID|NEWNS|NEWUTS|NEWIPC) -> cgroup에 넣기 -> 상태 저장 -> 자식에게 신호 -> waitpid
 *   자식: 신호 대기 -> sethostname -> mount private -> chroot -> /proc 마운트 -> execvp
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
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

struct child_args {
    const struct run_cfg *cfg;
    int sync_fd[2];               /* 부모가 준비를 끝낼 때까지 자식을 붙잡아 두는 파이프 */
};

/* 컨테이너 안에서 쓸 /proc, /tmp, /dev 를 만든다. 이 마운트는 컨테이너 안에서만 보인다 */
static void setup_mounts(void)
{
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
        perror("minic: mount /proc");

    mkdir("/tmp", 01777);
    if (mount("tmpfs", "/tmp", "tmpfs", 0, "size=16m") < 0)
        perror("minic: mount /tmp");

    mkdir("/dev", 0755);
    if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "size=1m,mode=755") == 0) {
        mknod("/dev/null",    S_IFCHR | 0666, makedev(1, 3));
        mknod("/dev/zero",    S_IFCHR | 0666, makedev(1, 5));
        mknod("/dev/random",  S_IFCHR | 0666, makedev(1, 8));
        mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
    }
}

/* clone()으로 만들어진 새 세계의 첫 프로세스 (여기서는 PID 1로 보인다) */
static int child_main(void *arg)
{
    struct child_args *a = arg;
    const struct run_cfg *c = a->cfg;
    char go;

    close(a->sync_fd[1]);
    if (read(a->sync_fd[0], &go, 1) != 1)      /* 부모가 죽었으면 그냥 종료 */
        _exit(1);
    close(a->sync_fd[0]);

    if (sethostname(c->name, strlen(c->name)) < 0)
        perror("minic: sethostname");

    /* 마운트가 호스트로 새지 않도록 전파를 끊는다 */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("minic: mount private");
        _exit(1);
    }
    if (chroot(c->rootfs) < 0 || chdir("/") < 0) {
        perror("minic: chroot");
        _exit(1);
    }
    setup_mounts();

    for (int fd = 3; fd < 256; fd++)           /* 호스트 쪽 파일 번호는 모두 닫는다 */
        close(fd);
    tty_take();                                 /* 포그라운드 실행이면 터미널을 넘겨받는다 */

    const char *term = getenv("TERM");
    char termbuf[64] = "xterm";
    if (term)
        snprintf(termbuf, sizeof termbuf, "%s", term);
    clearenv();
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/", 1);
    setenv("TERM", termbuf, 1);

    execvp(c->cmdv[0], c->cmdv);
    fprintf(stderr, "minic: '%s' 실행 실패: %s\n", c->cmdv[0], strerror(errno));
    _exit(127);
}

static void join_cmd(char *out, size_t n, char **cmdv)
{
    out[0] = '\0';
    for (int i = 0; cmdv[i]; i++) {
        size_t used = strlen(out);
        if (used + 2 >= n)
            break;
        snprintf(out + used, n - used, "%s%s", i ? " " : "", cmdv[i]);
    }
}

/* 컨테이너를 만들고 끝날 때까지 기다린다. report_fd가 있으면 시작한 PID를 알려준다 */
static int run_container(const struct run_cfg *cfg, int detached, int report_fd, int *status)
{
    struct cinfo ci;
    struct child_args a;
    int wst = 0;
    pid_t pid;

    memset(&ci, 0, sizeof ci);
    snprintf(ci.name, sizeof ci.name, "%s", cfg->name);
    snprintf(ci.rootfs, sizeof ci.rootfs, "%s", cfg->rootfs);
    join_cmd(ci.cmd, sizeof ci.cmd, cfg->cmdv);
    ci.mem_limit = cfg->mem_limit;
    ci.cpu_percent = cfg->cpu_percent;
    ci.pids_limit = cfg->pids_limit;
    ci.detached = detached;

    if (cg_create(cfg->name, cfg->mem_limit, cfg->cpu_percent, cfg->pids_limit) < 0) {
        fprintf(stderr, "minic: cgroup을 만들 수 없습니다 (%s)\n", cg_version());
        if (cfg->mem_limit || cfg->cpu_percent || cfg->pids_limit)
            goto fail;
    }

    if (pipe(a.sync_fd) < 0) {
        perror("pipe");
        goto fail;
    }
    a.cfg = cfg;

    char *stack = malloc(STACK_SIZE);
    if (!stack)
        goto fail;
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;
    pid = clone(child_main, stack + STACK_SIZE, flags, &a);
    free(stack);                                /* clone은 주소 공간을 복사하므로 바로 해제 가능 */
    if (pid < 0) {
        perror("minic: clone");
        close(a.sync_fd[0]);
        close(a.sync_fd[1]);
        goto fail;
    }
    close(a.sync_fd[0]);

    ci.pid = pid;
    ci.start = time(NULL);
    if (cg_add_pid(cfg->name, pid) < 0)
        fprintf(stderr, "minic: 경고: cgroup에 넣지 못했습니다 (제한이 적용되지 않음)\n");
    state_save(&ci);

    if (report_fd >= 0) {
        write_all(report_fd, &pid, sizeof pid);
        close(report_fd);
    }
    if (!detached) {
        signal(SIGINT, SIG_IGN);                /* Ctrl-C는 컨테이너 안 프로그램이 받게 한다 */
        printf("[minic] container '%s' started (host pid %d)\n", cfg->name, pid);
        fflush(stdout);
    }
    write_all(a.sync_fd[1], "1", 1);            /* 준비 끝: 자식 출발 */
    close(a.sync_fd[1]);
    while (waitpid(pid, &wst, 0) < 0 && errno == EINTR)
        ;
    if (!detached)
        tty_restore();                          /* 터미널을 다시 가져온다 */

    cg_destroy(cfg->name);
    if (detached) {
        state_write_exit(cfg->name, wst);
    } else {
        state_remove(cfg->name);
        printf("[minic] container '%s' exited (%s %d)\n", cfg->name,
               WIFEXITED(wst) ? "code" : "signal",
               WIFEXITED(wst) ? WEXITSTATUS(wst) : WTERMSIG(wst));
    }
    if (status)
        *status = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128 + WTERMSIG(wst);
    return 0;

fail:
    if (report_fd >= 0) {
        pid_t bad = -1;
        write_all(report_fd, &bad, sizeof bad);
        close(report_fd);
    }
    cg_destroy(cfg->name);
    return -1;
}

/* detached=1 이면 백그라운드(데몬)로 띄우고 바로 돌아온다 */
int container_start(const struct run_cfg *cfg, int detached, int *status)
{
    struct cinfo old;
    if (state_load(cfg->name, &old) == 0 && container_alive(&old)) {
        fprintf(stderr, "minic: '%s' 이름의 컨테이너가 이미 실행 중입니다\n", cfg->name);
        return -1;
    }
    if (state_prepare(cfg->name) < 0) {
        perror("minic: 상태 디렉터리 생성 실패");
        return -1;
    }

    if (!detached)
        return run_container(cfg, 0, -1, status);

    int rp[2];
    if (pipe(rp) < 0)
        return -1;
    pid_t sup = fork();
    if (sup < 0)
        return -1;

    if (sup > 0) {                              /* 사용자가 입력한 명령: 시작 결과만 확인하고 끝 */
        pid_t cpid = 0;
        close(rp[1]);
        ssize_t n = read(rp[0], &cpid, sizeof cpid);
        close(rp[0]);
        if (n != (ssize_t)sizeof cpid || cpid <= 0) {
            fprintf(stderr, "minic: 시작 실패 (minic logs %s 로 확인)\n", cfg->name);
            return -1;
        }
        printf("[minic] container '%s' started in background (host pid %d)\n",
               cfg->name, cpid);
        return 0;
    }

    /* 감시 프로세스: 터미널에서 분리되어 컨테이너가 끝날 때까지 기다린다 */
    close(rp[0]);
    setsid();
    char logp[PATH_MAX];
    state_path(logp, sizeof logp, cfg->name, "log");
    int lfd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int nfd = open("/dev/null", O_RDONLY);
    if (lfd >= 0) {
        dup2(lfd, 1);
        dup2(lfd, 2);
        close(lfd);
    }
    if (nfd >= 0) {
        dup2(nfd, 0);
        close(nfd);
    }
    run_container(cfg, 1, rp[1], NULL);
    _exit(0);
}

/* SIGTERM을 먼저 보내고, 제한 시간 안에 안 끝나면 SIGKILL로 강제 종료한다 */
int container_stop(const char *name, int timeout_sec)
{
    struct cinfo ci;
    if (state_load(name, &ci) < 0) {
        fprintf(stderr, "minic: '%s' 컨테이너를 찾을 수 없습니다\n", name);
        return -1;
    }
    if (!container_alive(&ci)) {
        printf("[minic] '%s' 는 이미 종료되었습니다\n", name);
        return 0;
    }

    printf("[minic] '%s' 에 SIGTERM 전송\n", name);
    kill(ci.pid, SIGTERM);
    for (int i = 0; i < timeout_sec * 10 && container_alive(&ci); i++)
        msleep(100);

    if (container_alive(&ci)) {
        printf("[minic] %d초 안에 끝나지 않아 SIGKILL 전송\n", timeout_sec);
        kill(ci.pid, SIGKILL);
        for (int i = 0; i < 30 && container_alive(&ci); i++)
            msleep(100);
    }
    if (container_alive(&ci)) {
        fprintf(stderr, "minic: '%s' 를 종료하지 못했습니다\n", name);
        return -1;
    }
    msleep(300);                                /* 감시 프로세스가 종료 기록을 남길 시간 */
    printf("[minic] '%s' stopped\n", name);
    return 0;
}

static void usage_run(void)
{
    fprintf(stderr,
        "사용법: minic run [옵션] <rootfs 폴더> [명령 [인자...]]\n"
        "  --name NAME   컨테이너 이름 (기본: c<PID>)\n"
        "  --mem SIZE    메모리 상한 (예: 64M, 1G)\n"
        "  --cpu PCT     CPU 상한, 코어 1개 기준 %% (예: 20)\n"
        "  --pids N      프로세스(스레드) 개수 상한\n"
        "  -d            백그라운드로 실행\n"
        "명령을 생략하면 /bin/sh 를 실행합니다.\n");
}

int cmd_run(int argc, char **argv)
{
    static char *default_cmd[] = { "/bin/sh", NULL };
    struct run_cfg cfg;
    struct stat st;
    int detached = 0, i;

    memset(&cfg, 0, sizeof cfg);
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-d")) {
            detached = 1;
        } else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            snprintf(cfg.name, sizeof cfg.name, "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--mem") && i + 1 < argc) {
            cfg.mem_limit = parse_size(argv[++i]);
            if (cfg.mem_limit < 0)
                die("--mem 값이 올바르지 않습니다 (예: 64M)");
        } else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) {
            cfg.cpu_percent = atoi(argv[++i]);
            if (cfg.cpu_percent <= 0 || cfg.cpu_percent > 10000)
                die("--cpu 값은 1 이상이어야 합니다");
        } else if (!strcmp(argv[i], "--pids") && i + 1 < argc) {
            cfg.pids_limit = atoi(argv[++i]);
            if (cfg.pids_limit <= 0)
                die("--pids 값은 1 이상이어야 합니다");
        } else {
            usage_run();
            return 1;
        }
    }
    if (i >= argc) {
        usage_run();
        return 1;
    }

    if (!realpath(argv[i], cfg.rootfs) || stat(cfg.rootfs, &st) < 0 || !S_ISDIR(st.st_mode))
        die("rootfs 폴더를 찾을 수 없습니다: %s (make rootfs 로 만들 수 있습니다)", argv[i]);
    cfg.cmdv = (i + 1 < argc) ? &argv[i + 1] : default_cmd;

    if (cfg.name[0] == '\0')
        snprintf(cfg.name, sizeof cfg.name, "c%d", getpid());
    if (!valid_name(cfg.name))
        die("이름은 영문, 숫자, -, _ 만 쓸 수 있습니다 (최대 32자)");

    int status = 0;
    if (container_start(&cfg, detached, &status) < 0)
        return 1;
    return detached ? 0 : status;
}

int cmd_stop(int argc, char **argv)
{
    int timeout = 3, i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-t")) {
        timeout = atoi(argv[i + 1]);
        i += 2;
    }
    if (i >= argc || timeout < 0) {
        fprintf(stderr, "사용법: minic stop [-t 초] <name>\n");
        return 1;
    }
    return container_stop(argv[i], timeout) == 0 ? 0 : 1;
}

int cmd_rm(int argc, char **argv)
{
    struct cinfo ci;
    if (argc != 2) {
        fprintf(stderr, "사용법: minic rm <name>\n");
        return 1;
    }
    if (state_load(argv[1], &ci) < 0)
        die("'%s' 컨테이너를 찾을 수 없습니다", argv[1]);
    if (container_alive(&ci))
        die("'%s' 는 실행 중입니다. 먼저 minic stop 하세요", argv[1]);
    state_remove(argv[1]);
    printf("[minic] '%s' removed\n", argv[1]);
    return 0;
}

/* 로그 파일을 화면에 출력. -f 를 주면 컨테이너가 끝날 때까지 계속 따라간다 */
int cmd_logs(int argc, char **argv)
{
    int follow = 0, i = 1;
    char path[PATH_MAX], buf[4096];
    struct cinfo ci;

    if (i < argc && !strcmp(argv[i], "-f")) {
        follow = 1;
        i++;
    }
    if (i >= argc) {
        fprintf(stderr, "사용법: minic logs [-f] <name>\n");
        return 1;
    }
    if (state_load(argv[i], &ci) < 0)
        die("'%s' 컨테이너를 찾을 수 없습니다", argv[i]);

    state_path(path, sizeof path, argv[i], "log");
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die("로그가 없습니다 (-d 로 실행한 컨테이너만 로그를 남깁니다)");

    for (;;) {
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0)
            write_all(STDOUT_FILENO, buf, n);
        if (!follow || !container_alive(&ci))
            break;
        msleep(500);
    }
    close(fd);
    return 0;
}
