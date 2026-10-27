# zeta-iwi-wifi - Wi-Fi for the Intel PRO/Wireless 2200BG on BeOS ZETA
#
#   on ZETA:     make            builds the kernel driver `iwi` and the daemon `iwid`
#                make install    copies them into place (see scripts/install.sh for the boot hook)
#   anywhere:    make test       builds and runs the crypto self-test (needs only a C compiler)
#
# ZETA 1.2.1 ships gcc 2.95 ("2.9-beos-000224"): the sources are plain C89-ish C and build with it.

CC      ?= gcc
KFLAGS   = -O1 -no-fpic -D_KERNEL_MODE=1 -Wall
DAEMON   = src/daemon/iwid.c src/daemon/wpa.c
KERNEL   = /boot/beos/system/kernel_intel

all: iwi iwid

# The driver must be linked WITHOUT the C start files (-nostdlib): crt pulls in __register_frame_info,
# which the kernel does not export, and the driver then silently fails to load.
iwi: src/kernel/iwi.c src/include/iwi_ioctl.h
	$(CC) -c $(KFLAGS) src/kernel/iwi.c -o iwi.o
	$(CC) -nostart -nostdlib -Xlinker -soname=iwi -o iwi iwi.o $(KERNEL)

iwid: $(DAEMON) src/daemon/wpa.h src/include/iwi_ioctl.h
	$(CC) -O1 -Wall -Isrc/include -Isrc/daemon -o iwid $(DAEMON)

ethtest: tests/ethtest.c
	$(CC) -o ethtest tests/ethtest.c

test_wpa: tests/test_wpa.c src/daemon/wpa.c src/daemon/wpa.h
	$(CC) -O1 -Wall -o test_wpa tests/test_wpa.c src/daemon/wpa.c

test: test_wpa
	./test_wpa

install: all
	sh scripts/install.sh

clean:
	rm -f iwi iwi.o iwid test_wpa ethtest

.PHONY: all test install clean
