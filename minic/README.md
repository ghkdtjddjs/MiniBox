# minic - 격리 효과를 눈으로 확인하는 미니 컨테이너

`minic`은 Docker 같은 컨테이너의 핵심 원리(namespace, chroot, cgroup)를 C로 직접 구현한 프로그램이다.
프로그램 하나를 "상자"에 가둬서 실행하고, 그 상자가 정말 격리되어 있는지를 **화면으로 비교하고 시험해서 보여준다.**

```
                     | HOST                           | CONTAINER 'box1'
  -------------------+--------------------------------+------------------------------
  hostname           | my-laptop                      | box1
  my PID             | 48211                          | 9
  visible processes  | 312                            | 3
  visible mounts     | 58                             | 3
  / entries          | 23: bin boot dev etc home ...  | 8: bin dev etc proc root tmp
  memory limit       | 15.6GB (RAM total)             | 64.0MB
```

## 1. 핵심 기능

| # | 명령 | 기능 | 사용 기술 |
|---|---|---|---|
| 1 | `run` | 격리된 컨테이너에서 프로그램 실행 (PID, 호스트명, 마운트, IPC 분리) | `clone`, `sethostname`, `mount`, `chroot`, `execvp` |
| 2 | `run --mem/--cpu/--pids` | 메모리, CPU, 프로세스 수 제한 | cgroup v2 (v1 자동 대체) |
| 3 | `run -d`, `logs` | 백그라운드 실행과 출력 로그 확인 | `fork`, `setsid`, `dup2` |
| 4 | `ps` | 컨테이너 목록, 상태, 실행 시간, 메모리 사용량 | `opendir/readdir`, `kill(pid, 0)` |
| 5 | `stop`, `rm` | 종료(SIGTERM → SIGKILL)와 기록 삭제 | `kill`, `waitpid` |
| 6 | `exec` | 실행 중인 컨테이너 안에 들어가서 명령 실행 | `setns`, `fchdir`, `chroot`, `fork` |
| 7 | `stats` | 컨테이너별 CPU, 메모리, 프로세스 수 실시간 모니터 | curses, cgroup 통계 |
| 8 | `compare` | 호스트와 컨테이너가 보는 세상을 나란히 비교 | `setns` + `/proc` 조사 |
| 9 | `demo` | 격리와 제한을 자동으로 시험하는 발표용 시연 | 위 기능 전체 |

## 2. 요구 사항

- Ubuntu 24.04 (cgroup v2). cgroup v1 시스템에서도 동작한다.
- **root 권한** (`sudo`). namespace 생성, chroot, mount, cgroup 설정에 필요하다.
- 패키지

```bash
sudo apt install build-essential libncurses-dev busybox-static
```

> 개발과 시연은 **가상 머신(VM)이나 WSL2**에서 하는 것을 권장한다. 학교 서버처럼 sudo가 없는 환경에서는 실행할 수 없다.

## 3. 빌드

```bash
make            # minic 실행 파일 생성
make rootfs     # 컨테이너용 루트 파일시스템(./rootfs) 생성 (busybox-static 사용)
```

busybox를 설치할 수 없으면 호스트의 명령어(sh, ls, ps, cat, sleep 등)와 라이브러리를 복사해서 만들 수 있다.

```bash
make rootfs-host
```

정리는 `make clean`(빌드 결과), `make distclean`(빌드 결과 + rootfs)으로 한다.

## 4. 사용법

### run: 컨테이너 실행

```bash
sudo ./minic run [--name N] [--mem 64M] [--cpu 50] [--pids 20] [-d] <rootfs> [명령 [인자...]]
```

| 옵션 | 의미 |
|---|---|
| `--name N` | 컨테이너 이름 (영문, 숫자, `-`, `_`, 최대 32자). 생략하면 `c<PID>` |
| `--mem SIZE` | 메모리 상한 (`512K`, `64M`, `1G`) |
| `--cpu PCT` | CPU 상한, 코어 1개 기준 % (`50`이면 반 코어) |
| `--pids N` | 컨테이너 안 프로세스(스레드) 개수 상한 |
| `-d` | 백그라운드 실행. 출력은 로그로 저장된다 |

명령을 생략하면 `/bin/sh`가 실행된다.

```bash
sudo ./minic run --name box1 --mem 64M ./rootfs            # 대화형 셸
sudo ./minic run -d --name web --cpu 30 ./rootfs sh -c 'while :; do date; sleep 1; done'
```

### ps: 목록

```bash
$ sudo ./minic ps
NAME           PID      STATUS      UPTIME   MEM(USED/LIMIT)       COMMAND
web            4903     running     0m05s    412.0KB/-             sh -c while :; do date; sleep 1; done
```

STATUS는 `running`, `exited(코드)`, `killed(시그널)`, `dead` 중 하나다.

### logs: 출력 보기

```bash
sudo ./minic logs web        # 지금까지의 출력
sudo ./minic logs -f web     # 컨테이너가 끝날 때까지 계속 따라가기
```

### exec: 컨테이너 안에서 명령 실행

```bash
sudo ./minic exec web            # 컨테이너 안에서 셸 열기
sudo ./minic exec web ps         # 컨테이너 안의 프로세스 목록
```

### stats: 실시간 모니터

```bash
sudo ./minic stats           # 1초마다 갱신, q로 종료
sudo ./minic stats --once    # 한 번만 출력
```

### compare: 격리 비교

```bash
sudo ./minic compare web
```

### stop, rm

```bash
sudo ./minic stop web          # SIGTERM 후 3초 안에 안 끝나면 SIGKILL
sudo ./minic stop -t 10 web    # 기다리는 시간 변경
sudo ./minic rm web            # 종료된 컨테이너 기록 삭제
```

### demo: 자동 시연

```bash
sudo ./minic demo            # 단계마다 Enter로 진행 (발표용)
sudo ./minic demo --auto     # 멈추지 않고 끝까지
```

| 단계 | 내용 | 기대 결과 |
|---|---|---|
| 1 | 메모리 32MB, CPU 20%, 프로세스 16개로 제한한 컨테이너 시작 | `sleep`이 PID 1로 실행 |
| 2 | 호스트와 컨테이너 비교 | 호스트명, PID, 프로세스 수, 마운트, 파일 목록이 다름 |
| 3 | 1MB씩 메모리 할당 | 약 32MB에서 커널이 SIGKILL로 종료 |
| 4 | 자식 프로세스 40개 생성 시도 | 일부만 성공하고 나머지는 EAGAIN으로 실패 |
| 5 | 3초 동안 CPU 최대 사용 | 측정된 사용률이 약 20% |
| 6 | 종료와 정리 | cgroup과 상태 파일 삭제 |

## 5. 동작 원리

### 컨테이너 시작 (`container.c`)

```
부모(minic)                                 자식 (새 namespace 안, PID 1)
  clone(NEWPID|NEWNS|NEWUTS|NEWIPC) ───────▶ 파이프에서 "출발" 신호 대기
  cgroup에 자식 PID 등록                      │
  상태 파일 저장                              │
  파이프에 "출발" 쓰기 ──────────────────────▶ sethostname(이름)
                                              mount("/", MS_REC|MS_PRIVATE)  마운트가 호스트로 새지 않게
                                              chroot(rootfs), chdir("/")
                                              /proc, /tmp, /dev 마운트
                                              execvp(명령)
  waitpid() 로 종료 대기
  cgroup 삭제, 종료 기록 저장
```

파이프로 자식을 붙잡아 두는 이유는, 자식이 프로그램을 실행하기 **전에** cgroup에 들어가 있어야 처음부터 제한을 받기 때문이다.

### 자원 제한 (`cgroup.c`)

| 제한 | cgroup v2 파일 | cgroup v1 파일 |
|---|---|---|
| 메모리 | `memory.max`, `memory.swap.max` | `memory.limit_in_bytes` |
| CPU | `cpu.max` (`20000 100000` = 20%) | `cpu.cfs_quota_us`, `cpu.cfs_period_us` |
| 프로세스 수 | `pids.max` | `pids.max` |
| 사용량 측정 | `memory.current`, `cpu.stat`, `pids.current` | `memory.usage_in_bytes`, `cpuacct.usage`, `pids.current` |

`/sys/fs/cgroup/cgroup.controllers`가 있으면 v2, 없으면 v1로 자동 판단한다.

### exec (`enter.c`)

1. `/proc/<pid>/ns/{pid,ipc,uts,mnt}`와 `/proc/<pid>/root`를 먼저 연다.
2. 컨테이너의 cgroup에 합류한다 (chroot 전에 해야 `/sys/fs/cgroup`이 보인다).
3. `setns()`로 각 namespace에 들어간다.
4. `fchdir(root)` + `chroot(".")`로 컨테이너 파일시스템으로 이동한다.
5. PID namespace는 setns 이후 **자식부터** 적용되므로 `fork()`를 한 번 더 한다.

### 알아두면 좋은 동작

- **PID 1은 SIGTERM을 무시한다.** namespace의 init 프로세스는 핸들러를 등록하지 않은 시그널을 커널이 무시한다. 그래서 `stop`은 SIGTERM을 먼저 보내고, 끝나지 않으면 SIGKILL을 보낸다. Docker에서 같은 이유로 `tini` 같은 init을 쓴다.
- **터미널 넘기기.** 포그라운드로 실행한 셸은 호스트의 프로세스 그룹을 볼 수 없으므로, 컨테이너가 새 프로세스 그룹을 만들어 `tcsetpgrp()`로 터미널을 넘겨받고, 끝나면 minic이 되돌려받는다. 그래서 Ctrl-C가 컨테이너 안의 명령에만 전달된다.

## 6. 사용한 시스템콜

| 분류 | 시스템콜 |
|---|---|
| 프로세스 | `clone`, `fork`, `execvp`, `waitpid`, `wait`, `kill`, `setsid`, `setpgid`, `getpid` |
| 격리 | `setns`, `sethostname`, `chroot`, `fchdir`, `chdir`, `mount` |
| 파일 | `open`, `read`, `write`, `close`, `mkdir`, `rmdir`, `unlink`, `mknod`, `stat`, `access` |
| 디렉터리 | `opendir`, `readdir`, `closedir` |
| IPC, 시그널 | `pipe`, `dup2`, `signal` |
| 터미널, 시간 | `tcsetpgrp`, `nanosleep`, `clock_gettime` |

## 7. 파일 구조

| 파일 | 내용 |
|---|---|
| `main.c` | 하위 명령 분기, 도움말 |
| `container.c` | `run`, `stop`, `rm`, `logs` (clone, chroot, 마운트) |
| `enter.c` | 실행 중인 컨테이너 진입 (`exec`, setns) |
| `cgroup.c` | 자원 제한과 사용량 측정 (v2/v1) |
| `state.c` | 상태 파일 저장과 조회 (`/var/run/minic/<name>/`) |
| `ui.c` | `ps`, `stats`(curses), `compare` |
| `demo.c` | 자동 시연 |
| `util.c` | 파일 입출력, 단위 변환, 터미널 처리 |
| `minic.h` | 공통 구조체와 함수 선언 |

## 8. 한계와 주의

- 학습용 구현이라 **보안 격리가 완전하지 않다.** root 권한을 그대로 갖고 있고 capability를 제한하지 않으며, chroot는 root 권한으로 탈출이 가능하다.
- 네트워크 namespace와 user namespace는 분리하지 않는다.
- root로 마운트와 cgroup을 다루므로 **VM에서 개발하고 스냅샷을 찍어 두는 것을 권장한다.**

## 9. 문제 해결

| 증상 | 해결 |
|---|---|
| `root 권한이 필요합니다` | `sudo ./minic ...`로 실행 |
| `rootfs 폴더를 찾을 수 없습니다` | `make rootfs` 또는 `make rootfs-host` |
| `cgroup을 만들 수 없습니다` | `/sys/fs/cgroup`이 마운트되어 있는지 확인. WSL2라면 `/etc/wsl.conf`에 `[boot]` `systemd=true`를 넣고 재시작해 보기 |
| `경고: ... 제한을 설정하지 못했습니다` | 해당 cgroup 컨트롤러가 꺼져 있음. v2는 `cat /sys/fs/cgroup/cgroup.subtree_control`로 확인 |
| demo 1단계에서 바로 종료 | rootfs 안에 `sleep`이 있는지 확인 |
