bin_PROGRAMS = ptyterm ptywrap biopen pbuf

AM_CFLAGS = -O2 -Wall -Werror

ptyterm_SOURCES = ptyterm.c
ptywrap_SOURCES = ptywrap.c
biopen_SOURCES = biopen.c
pbuf_SOURCES = pbuf.c

TESTS = \
	test-ptyterm-help.sh \
	test-ptywrap-help.sh \
	test-pbuf-help.sh \
	test-pbuf-copy.sh

EXTRA_DIST = $(TESTS)
