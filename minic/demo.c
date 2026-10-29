/* demo.c - 격리 효과를 눈으로 확인하는 자동 시연 (minic demo)
 *
 *   1. 제한(메모리 32MB, CPU 20%, 프로세스 16개)을 건 컨테이너 시작
 *   2. 호스트와 컨테이너가 보는 세상 비교 (namespace 효과)
 *   3. 메모리를 계속 할당 -> 한도에서 커널이 강제 종료 (memory 제한)
 *   4. 프로세스를 40개 만들려고 시도 -> 일부만 성공 (pids 제한)
 *   5. 3초 동안 CPU를 최대로 사용 -> 실제 사용률 측정 (cpu 제한)
 *   6. 종료와 정리
 */
#include "minic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define DEMO_NAME  "minic-demo"
#define DEMO_MEM   (32LL << 20)
#define DEMO_CPU   20
#define DEMO_PIDS  16
#define FORK_TRY   40

static int auto_mode;

static void wait_enter(void)
{
    if (auto_mode || !isatty(STDIN_FILENO)) {
        printf("\n");
        return;
    }
    printf("\n  (Enter를 누르면 다음 단계로) ");
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

static void step(int n, const char *title)
{
    printf("\n==================================================================\n");
    printf(" STEP %d. %s\n", n, title);
    printf("==================================================================\n");
}

/* ---------------- 컨테이너 안에서 실행되는 시험 함수들 ---------------- */

/* 1MB씩 할당하고 실제로 써서(페이지를 진짜로 사용) 진행 상황을 파이프로 보고 */
static int memhog_fn(void *arg)
{
    int fd = *(int *)arg;
    for (int mb = 1; mb <= 256; mb++) {
        volatile char *p = malloc(1 << 20);
        if (!p)
            break;
        for (int i = 0; i < (1 << 20); i += 4096)
            p[i] = 1;
        if (write(fd, &mb, sizeof mb) != (ssize_t)sizeof mb)
            break;
    }
    return 0;
}

/* 자식 프로세스를 FORK_TRY개 만들려고 시도하고 성공/실패 개수를 보고 */
static int forktest_fn(void *arg)
{
    int fd = *(int *)arg;
    int res[2] = { 0, 0 };
    for (int i = 0; i < FORK_TRY; i++) {
        pid_t c = fork();
        if (c == 0) {
            sleep(3);
            _exit(0);
        }
        if (c > 0)
            res[0]++;
        else
            res[1]++;
    }
    if (write(fd, res, sizeof res) != (ssize_t)sizeof res)
        return 1;
    while (wait(NULL) > 0)
        ;
    return 0;
}

/* 3초 동안 CPU를 쉬지 않고 사용 */
static int cpuburn_fn(void *arg)
{
    (void)arg;
    volatile unsigned long x = 0;
    long long end = now_usec() + 3000000LL;
    while (now_usec() < end)
        x++;
    return 0;
}

/* ---------------- 단계별 시연 ---------------- */

static int test_memory(const struct cinfo *ci)
{
    int pfd[2], mb, last = 0;
    if (pipe(pfd) < 0)
        return -1;

    printf("  컨테이너 안에서 1MB씩 메모리를 계속 할당합니다 (최대 256MB 시도)\n");
    pid_t p = spawn_in_container(ci, memhog_fn, &pfd[1]);
    close(pfd[1]);
    if (p < 0) {
        close(pfd[0]);
        return -1;
    }
    while (read(pfd[0], &mb, sizeof mb) == (ssize_t)sizeof mb) {
        last = mb;
        printf("\r  할당량: %3d MB ", mb);
        fflush(stdout);
    }
    close(pfd[0]);
    int code = wait_code(p);
    printf("\n");

    if (code == 128 + SIGKILL) {
        printf("  결과: %d MB 근처에서 커널이 프로세스를 강제 종료했습니다 (SIGKILL, OOM)\n", last);
        printf("        -> 메모리 한도 32MB 제한이 실제로 동작합니다  [PASS]\n");
        return 0;
    }
    printf("  결과: %d MB까지 할당됨 (종료 코드 %d) -> 메모리 제한이 적용되지 않았습니다  [FAIL]\n",
           last, code);
    return 1;
}

static int test_pids(const struct cinfo *ci)
{
    int pfd[2], res[2] = { 0, 0 };
    if (pipe(pfd) < 0)
        return -1;

    printf("  컨테이너 안에서 자식 프로세스를 %d개 만들려고 시도합니다\n", FORK_TRY);
    pid_t p = spawn_in_container(ci, forktest_fn, &pfd[1]);
    close(pfd[1]);
    if (p < 0) {
        close(pfd[0]);
        return -1;
    }
    ssize_t n = read(pfd[0], res, sizeof res);
    close(pfd[0]);
    wait_code(p);
    if (n != (ssize_t)sizeof res)
        return -1;

    printf("  결과: fork 성공 %d개, 실패 %d개 (컨테이너 전체 프로세스 상한 %d개)\n",
           res[0], res[1], DEMO_PIDS);
    if (res[1] > 0) {
        printf("        -> 상한을 넘는 fork는 커널이 거부합니다 (EAGAIN)  [PASS]\n");
        return 0;
    }
    printf("        -> 모든 fork가 성공했습니다. pids 제한이 적용되지 않았습니다  [FAIL]\n");
    return 1;
}

static int test_cpu(const struct cinfo *ci)
{
    printf("  컨테이너 안에서 3초 동안 CPU를 최대로 사용합니다...\n");
    long long u0 = cg_cpu_usec(ci->name), t0 = now_usec();
    pid_t p = spawn_in_container(ci, cpuburn_fn, NULL);
    if (p < 0)
        return -1;
    wait_code(p);
    long long u1 = cg_cpu_usec(ci->name), t1 = now_usec();

    if (u0 < 0 || u1 < 0) {
        printf("  결과: CPU 사용량을 읽을 수 없습니다  [SKIP]\n");
        return 1;
    }
    double pct = (u1 - u0) * 100.0 / (double)(t1 - t0);
    printf("  결과: 측정된 CPU 사용률 %.1f%% (제한 %d%%, 제한이 없으면 약 100%%)\n", pct, DEMO_CPU);
    if (pct <= DEMO_CPU * 1.5) {
        printf("        -> CPU 제한이 동작합니다  [PASS]\n");
        return 0;
    }
    printf("        -> 제한보다 많이 사용했습니다. CPU 제한이 적용되지 않았습니다  [FAIL]\n");
    return 1;
}

int cmd_demo(int argc, char **argv)
{
    static char *demo_cmd[] = { "sleep", "3600", NULL };
    const char *rootfs = "./rootfs";
    struct run_cfg cfg;
    struct cinfo ci;
    int fails = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--auto"))
            auto_mode = 1;
        else
            rootfs = argv[i];
    }

    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.name, sizeof cfg.name, "%s", DEMO_NAME);
    if (!realpath(rootfs, cfg.rootfs))
        die("rootfs 폴더를 찾을 수 없습니다: %s (make rootfs 로 만들 수 있습니다)", rootfs);
    cfg.cmdv = demo_cmd;
    cfg.mem_limit = DEMO_MEM;
    cfg.cpu_percent = DEMO_CPU;
    cfg.pids_limit = DEMO_PIDS;

    printf("\n minic demo - 컨테이너 격리 효과를 눈으로 확인합니다 (%s)\n", cg_version());

    step(1, "제한을 건 컨테이너 시작 (메모리 32MB, CPU 20%, 프로세스 16개)");
    if (state_load(DEMO_NAME, &ci) == 0 && container_alive(&ci))
        container_stop(DEMO_NAME, 1);
    if (container_start(&cfg, 1, NULL) < 0)
        return 1;
    msleep(500);
    if (state_load(DEMO_NAME, &ci) < 0 || !container_alive(&ci)) {
        fprintf(stderr, "minic: 컨테이너가 바로 종료되었습니다. rootfs 안에 sleep 명령이 있는지 확인하세요.\n");
        return 1;
    }
    printf("  컨테이너 안에서 'sleep 3600' 이 PID 1로 실행 중입니다\n");
    wait_enter();

    step(2, "격리 확인: 호스트와 컨테이너는 서로 다른 세상을 본다 (namespace)");
    if (compare_container(DEMO_NAME) < 0)
        fails++;
    wait_enter();

    step(3, "메모리 제한 확인 (cgroup memory)");
    if (test_memory(&ci) != 0)
        fails++;
    wait_enter();

    step(4, "프로세스 수 제한 확인 (cgroup pids)");
    if (test_pids(&ci) != 0)
        fails++;
    wait_enter();

    step(5, "CPU 제한 확인 (cgroup cpu)");
    if (test_cpu(&ci) != 0)
        fails++;
    wait_enter();

    step(6, "종료와 정리");
    container_stop(DEMO_NAME, 1);
    state_remove(DEMO_NAME);
    printf("\n 시연 완료: %s\n\n", fails ? "일부 항목이 통과하지 못했습니다" : "모든 항목 통과");
    return fails ? 1 : 0;
}
