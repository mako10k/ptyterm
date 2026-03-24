#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define PACKAGE_STRING "ptytermd"
#endif

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long parse_size(const char *arg, const char *optname) {
  char *end;
  unsigned long value;
  unsigned long scale = 1;

  errno = 0;
  value = strtoul(arg, &end, 0);
  if (errno != 0 || arg == end || value == 0) {
    fprintf(stderr, "invalid %s: %s\n", optname, arg);
    exit(EXIT_FAILURE);
  }

  if (*end != '\0') {
    if (end[1] != '\0') {
      fprintf(stderr, "invalid %s: %s\n", optname, arg);
      exit(EXIT_FAILURE);
    }
    switch (*end) {
    case 'k':
    case 'K':
      scale = 1024UL;
      break;
    case 'm':
    case 'M':
      scale = 1024UL * 1024UL;
      break;
    default:
      fprintf(stderr, "invalid %s: %s\n", optname, arg);
      exit(EXIT_FAILURE);
    }
  }

  if (value > ULONG_MAX / scale) {
    fprintf(stderr, "%s too large: %s\n", optname, arg);
    exit(EXIT_FAILURE);
  }

  return value * scale;
}

int main(int argc, char *const argv[]) {
  const char *socket_path = NULL;
  const char *overflow = "drop";
  unsigned long output_buffer = 4096UL;

  setlocale(LC_ALL, "");
  for (;;) {
    int c, optindex;
    static struct option longopts[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {"socket", required_argument, NULL, 's'},
        {"output-buffer", required_argument, NULL, 'b'},
        {"overflow", required_argument, NULL, 'o'},
        {NULL, 0, NULL, 0}};

    c = getopt_long(argc, argv, "hVs:b:o:", longopts, &optindex);
    if (c == -1)
      break;

    switch (c) {
    case 'h':
    case 'V':
      printf("%s\n", PACKAGE_STRING);
      if (c != 'h')
        exit(EXIT_SUCCESS);
      printf("\n");
      printf("Usage:\n");
      printf("  %s [options]\n", argv[0]);
      printf("\n");
      printf("Options:\n");
      printf("  --socket=PATH              control socket path\n");
      printf("  --output-buffer=SIZE       per-session output buffer size\n");
      printf("  --overflow=drop|pause      output buffer overflow policy\n");
      printf("  -V, --version              print version and exit\n");
      printf("  -h, --help                 print this usage and exit\n");
      printf("\n");
      exit(EXIT_SUCCESS);
    case 's':
      socket_path = optarg;
      break;
    case 'b':
      output_buffer = parse_size(optarg, "output-buffer");
      break;
    case 'o':
      if (strcmp(optarg, "drop") != 0 && strcmp(optarg, "pause") != 0) {
        fprintf(stderr, "invalid overflow: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      overflow = optarg;
      break;
    default:
      exit(EXIT_FAILURE);
    }
  }

  if (optind != argc) {
    fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
    exit(EXIT_FAILURE);
  }

  (void)socket_path;
  (void)overflow;
  (void)output_buffer;

  fprintf(stderr, "%s: daemon runtime is not implemented yet\n", argv[0]);
  return EXIT_FAILURE;
}