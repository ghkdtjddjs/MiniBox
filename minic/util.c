/* util.c - 파일 입출력, 문자열 변환 같은 공통 도구 */
#include "minic.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>

void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "minic: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* 파일에 문자열을 한 번에 쓴다. cgroup 파일은 write()를 한 번만 호출해야 한다. */
int write_str(const char *path, const char *s, int create)
{
    int flags = O_WRONLY;
    if (create)
        flags |= O_CREAT | O_TRUNC;
    int fd = open(path, flags, 0644);
    if (fd < 0)
        return -1;
    size_t len = strlen(s);
    ssize_t w = write(fd, s, len);
    int saved = errno;
    close(fd);
    if (w != (ssize_t)len) {
        errno = (w < 0) ? saved : EIO;
        return -1;
    }
    return 0;
}

/* write()가 일부만 쓰고 돌아와도 끝까지 쓴다 */
int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= w;
    }
    return 0;
}

ssize_t read_str(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

long long read_ll(const char *path)
{
    char buf[64];
    if (read_str(path, buf, sizeof buf) < 0)
        return -1;
    return strtoll(buf, NULL, 10);
}

/* "64M", "512K", "1G", "1048576" 같은 문자열을 byte로 변환. 실패하면 -1 */
long long parse_size(const char *s)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || end == s || v <= 0)
        return -1;
    switch (toupper((unsigned char)*end)) {
    case '\0': return v;
    case 'K':  return v << 10;
    case 'M':  return v << 20;
    case 'G':  return v << 30;
    default:   return -1;
    }
}

/* 컨테이너 이름은 경로에 쓰이므로 영문, 숫자, -, _ 만 허용한다 */
int valid_name(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > 32)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_')
            return 0;
    return 1;
}

void fmt_bytes(long long b, char *out, size_t n)
{
    if (b < 0)
        snprintf(out, n, "-");
    else if (b < 1024)
        snprintf(out, n, "%lldB", b);
    else if (b < (1LL << 20))
        snprintf(out, n, "%.1fKB", b / 1024.0);
    else if (b < (1LL << 30))
        snprintf(out, n, "%.1fMB", b / 1048576.0);
    else
        snprintf(out, n, "%.1fGB", b / 1073741824.0);
}

void fmt_uptime(time_t start, char *out, size_t n)
{
    long s = (long)(time(NULL) - start);
    if (s < 0)
        s = 0;
    if (s < 60)
        snprintf(out, n, "%lds", s);
    else if (s < 3600)
        snprintf(out, n, "%ldm%02lds", s / 60, s % 60);
    else
        snprintf(out, n, "%ldh%02ldm", s / 3600, (s % 3600) / 60);
}

/* 프로세스가 살아 있는지 확인한다. 좀비는 죽은 것으로 본다 */
int proc_alive(pid_t pid)
{
    if (pid <= 0)
        return 0;
    if (kill(pid, 0) < 0 && errno != EPERM)
        return 0;

    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (read_str(path, buf, sizeof buf) < 0)
        return 0;
    char *p = strrchr(buf, ')');     /* 프로세스 이름에 공백이 있어도 안전하게 */
    if (p && p[1] == ' ' && p[2] == 'Z')
        return 0;
    return 1;
}

/* 터미널 전면(foreground) 프로세스 그룹 넘기기
 * 컨테이너 안의 셸은 호스트의 프로세스 그룹을 볼 수 없으므로,
 * 새 프로세스 그룹을 만들어 터미널을 넘겨줘야 Ctrl-C와 종료가 정상 동작한다. */
void tty_take(void)
{
    if (!isatty(STDIN_FILENO))
        return;
    setpgid(0, 0);
    signal(SIGTTOU, SIG_IGN);          /* 배경 그룹에서 tcsetpgrp 하면 SIGTTOU로 멈추므로 */
    tcsetpgrp(STDIN_FILENO, getpgrp());
    signal(SIGTTOU, SIG_DFL);          /* SIG_IGN은 exec 후에도 남으므로 원래대로 */
}

void tty_restore(void)
{
    if (!isatty(STDIN_FILENO))
        return;
    signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(STDIN_FILENO, getpgrp());
    signal(SIGTTOU, SIG_DFL);
}

long long now_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
