/* main.c - minic: 격리 효과를 눈으로 확인하는 미니 컨테이너
 *
 * 하위 명령을 골라서 해당 함수로 넘긴다.
 */
#include "minic.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

struct command {
    const char *name;
    int (*fn)(int, char **);
    const char *help;
};

static const struct command COMMANDS[] = {
    { "run",     cmd_run,     "컨테이너 실행      run [--name N] [--mem 64M] [--cpu 20] [--pids 16] [-d] <rootfs> [cmd...]" },
    { "ps",      cmd_ps,      "컨테이너 목록      ps" },
    { "stop",    cmd_stop,    "컨테이너 종료      stop [-t 초] <name>" },
    { "rm",      cmd_rm,      "종료된 기록 삭제   rm <name>" },
    { "logs",    cmd_logs,    "출력 로그 보기     logs [-f] <name>" },
    { "exec",    cmd_exec,    "내부에서 명령 실행 exec <name> [cmd...]" },
    { "stats",   cmd_stats,   "실시간 자원 모니터 stats [--once]" },
    { "compare", cmd_compare, "호스트와 격리 비교 compare <name>" },
    { "demo",    cmd_demo,    "자동 시연          demo [--auto] [rootfs]" },
};
#define NCMD (sizeof COMMANDS / sizeof COMMANDS[0])

static void usage(void)
{
    printf("minic - 격리 효과를 눈으로 확인하는 미니 컨테이너\n\n");
    printf("사용법: sudo ./minic <명령> [옵션]\n\n");
    for (size_t i = 0; i < NCMD; i++)
        printf("  %-8s %s\n", COMMANDS[i].name, COMMANDS[i].help);
    printf("\n예시:\n");
    printf("  sudo ./minic run --name box1 --mem 64M ./rootfs /bin/sh\n");
    printf("  sudo ./minic demo\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "-h") ||
        !strcmp(argv[1], "--help")) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    signal(SIGPIPE, SIG_IGN);

    for (size_t i = 0; i < NCMD; i++) {
        if (strcmp(argv[1], COMMANDS[i].name) != 0)
            continue;
        if (geteuid() != 0)
            die("root 권한이 필요합니다. sudo ./minic %s ... 로 실행하세요", argv[1]);
        return COMMANDS[i].fn(argc - 1, argv + 1);
    }
    fprintf(stderr, "minic: 알 수 없는 명령 '%s'\n\n", argv[1]);
    usage();
    return 1;
}
