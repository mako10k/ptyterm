bin_PROGRAMS = ptyterm ptywrap biopen pbuf ptytermd

AM_CFLAGS = -O2 -Wall -Werror

ptyterm_SOURCES = ptyterm.c ptyterm-control.c
ptywrap_SOURCES = ptywrap.c
biopen_SOURCES = biopen.c
pbuf_SOURCES = pbuf.c
ptytermd_SOURCES = ptytermd.c ptyterm-control.c

TESTS = \
	test-ptyterm-help.sh \
	test-ptytermd-help.sh \
	test-ptyterm-buffer-info.sh \
	test-ptyterm-create.sh \
	test-ptyterm-list.sh \
	test-ptywrap-help.sh \
	test-pbuf-help.sh \
	test-pbuf-copy.sh

EXTRA_DIST = $(TESTS)
