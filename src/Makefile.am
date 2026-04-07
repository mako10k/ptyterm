bin_PROGRAMS = ptyterm ptywrap biopen pbuf ptytermd

AM_CFLAGS = -O2 -Wall -Werror

ptyterm_SOURCES = ptyterm.c ptyterm-control.c
ptywrap_SOURCES = ptywrap.c
biopen_SOURCES = biopen.c
pbuf_SOURCES = pbuf.c
ptytermd_SOURCES = ptytermd.c ptyterm-control.c
noinst_HEADERS = ptyterm-control.h

TESTS = \
	test-ptyterm-help.sh \
	test-ptyterm-help-errors.sh \
	test-ptyterm-help-format.sh \
	test-ptyterm-recv-peek-errors.sh \
	test-ptyterm-recv-wait.sh \
	test-ptyterm-daemon-lifecycle.sh \
	test-ptyterm-status-format.sh \
	test-ptyterm-attach-nonblock.sh \
	test-ptyterm-attach-resize.sh \
	test-ptytermd-help.sh \
	test-ptyterm-attach-detach.sh \
	test-ptyterm-buffer-info.sh \
	test-ptyterm-create.sh \
	test-ptyterm-list.sh \
	test-ptyterm-resize.sh \
	test-ptyterm-send-recv.sh \
	test-ptywrap-help.sh \
	test-biopen-help.sh \
	test-pbuf-help.sh \
	test-pbuf-copy.sh

EXTRA_DIST = $(TESTS)
