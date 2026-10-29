# minic - 미니 컨테이너 빌드
#   make            : minic 빌드
#   make rootfs     : busybox(정적 빌드)로 컨테이너용 루트 파일시스템 생성
#   make rootfs-host: busybox가 없을 때 호스트의 명령어와 라이브러리를 복사해서 생성
#   make clean      : 빌드 결과 삭제
#   make distclean  : 빌드 결과 + rootfs 삭제

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
CFLAGS  += -D_GNU_SOURCE
LDLIBS   = -lncurses

SRCS = main.c util.c cgroup.c state.c container.c enter.c ui.c demo.c
OBJS = $(SRCS:.c=.o)

ROOTFS   = rootfs
BUSYBOX ?= $(shell command -v busybox 2>/dev/null)
HOSTBINS = sh ls cat ps sleep hostname echo mount

.PHONY: all clean distclean rootfs rootfs-host

all: minic

minic: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c minic.h
	$(CC) $(CFLAGS) -c $<

rootfs:
	@if [ -z "$(BUSYBOX)" ]; then \
		echo "busybox가 없습니다: sudo apt install busybox-static"; exit 1; fi
	@if ldd "$(BUSYBOX)" >/dev/null 2>&1; then \
		echo "정적(static) busybox가 필요합니다: sudo apt install busybox-static"; exit 1; fi
	mkdir -p $(ROOTFS)/bin $(ROOTFS)/proc $(ROOTFS)/tmp $(ROOTFS)/dev $(ROOTFS)/etc $(ROOTFS)/root
	cp "$(BUSYBOX)" $(ROOTFS)/bin/busybox
	for app in $$($(ROOTFS)/bin/busybox --list); do \
		[ -e $(ROOTFS)/bin/$$app ] || ln -s busybox $(ROOTFS)/bin/$$app; \
	done
	echo "root:x:0:0:root:/root:/bin/sh" > $(ROOTFS)/etc/passwd
	@echo "rootfs 준비 완료: ./$(ROOTFS)"

rootfs-host:
	mkdir -p $(ROOTFS)/bin $(ROOTFS)/proc $(ROOTFS)/tmp $(ROOTFS)/dev $(ROOTFS)/etc $(ROOTFS)/root
	for b in $(HOSTBINS); do \
		p=$$(command -v $$b) || continue; \
		cp -L "$$p" $(ROOTFS)/bin/; \
		for lib in $$(ldd "$$p" 2>/dev/null | grep -o '/[^ ]*'); do \
			mkdir -p $(ROOTFS)$$(dirname $$lib); \
			cp -Ln "$$lib" $(ROOTFS)$$lib; \
		done; \
	done
	echo "root:x:0:0:root:/root:/bin/sh" > $(ROOTFS)/etc/passwd
	@echo "rootfs 준비 완료 (호스트 명령어 복사): ./$(ROOTFS)"

clean:
	rm -f minic $(OBJS)

distclean: clean
	rm -rf $(ROOTFS)
