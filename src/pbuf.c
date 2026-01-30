#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define PACKAGE_STRING "ptyterm"
#endif

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

int pbuf(int ifd, int ofd, size_t bufsize) {
  struct stat ist, ost;
  if (fstat(ifd, &ist) == -1) {
    perror("fstat");
    return -1;
  }
  if (fstat(ofd, &ost) == -1) {
    perror("fstat");
    return -1;
  }
  if (bufsize == 0) {
    bufsize = ist.st_blksize;
    if (bufsize < ost.st_blksize) {
      bufsize = ost.st_blksize;
    }
  }
  if (bufsize == 0) {
    bufsize = 1;
  }
  size_t pagesize = sysconf(_SC_PAGE_SIZE);
  bufsize += pagesize - 1;
  bufsize &= ~(pagesize - 1);

  char *buf = malloc(bufsize);
  if (buf == NULL) {
    perror("malloc");
    return -1;
  }

  size_t pos = 0, len = 0;
  while (1) {
    fd_set rfds, wfds;
    int maxfd = -1;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (len == 0) {
      pos = 0;
    }
    if (ifd >= 0 && len < bufsize) {
      FD_SET(ifd, &rfds);
      if (maxfd < ifd) {
        maxfd = ifd;
      }
    }
    if (ofd >= 0 && len > 0) {
      FD_SET(ofd, &wfds);
      if (maxfd < ofd) {
        maxfd = ofd;
      }
    }
    if (maxfd == -1) {
      free(buf);
      return 0;
    }
    int ret = select(maxfd + 1, &rfds, &wfds, NULL, NULL);
    if (ret == -1) {
      perror("select");
      free(buf);
      return -1;
    }

    // Read from input
    if (ifd >= 0 && FD_ISSET(ifd, &rfds)) {
      ssize_t siz;

      if (pos == 0 || pos + len >= bufsize) {
        siz = read(ifd, buf + pos + len, bufsize - len);
        if (siz == -1) {
          perror("read");
          free(buf);
          return -1;
        }
      } else {
        struct iovec iov[2] = {
            {buf + pos + len, bufsize - pos - len},
            {buf, pos},
        };
        siz = readv(ifd, iov, 2);
        if (siz == -1) {
          perror("readv");
          free(buf);
          return -1;
        }
      }

      if (siz == 0) {
        ifd = -1;
      }

      len += siz;
      continue;
    }

    // Write to output
    if (ofd >= 0 && FD_ISSET(ofd, &wfds)) {
      ssize_t siz;

      if (pos + len < bufsize) {
        siz = write(ofd, buf + pos, len);
        if (siz == -1) {
          perror("write");
          free(buf);
          return -1;
        }
      } else {
        struct iovec iov[2] = {
            {buf + pos, bufsize - pos},
            {buf, len - bufsize + pos},
        };
        siz = writev(ofd, iov, 2);
        if (siz == -1) {
          perror("writev");
          free(buf);
          return -1;
        }
      }
      len -= siz;
    }
  }
  free(buf);
  return 0;
}

int main(int argc, char *argv[]) {

  struct option longopts[] = {
      {"buffer-size", required_argument, NULL, 'b'},
      {"version", no_argument, NULL, 'V'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };
  unsigned buffer_size = 0;
  int opt;
  char *p;
  while ((opt = getopt_long(argc, argv, "b:Vh", longopts, NULL)) != -1) {
    switch (opt) {
    case 'b':
      buffer_size = strtoul(optarg, &p, 0);
      if (optarg == p) {
        fprintf(stderr, "invalid buffer size: %s\n", optarg);
        return EXIT_FAILURE;
      }
      switch (*p) {
      case 'k':
      case 'K':
        buffer_size *= 1024;
        p++;
        break;
      case 'm':
      case 'M':
        buffer_size *= 1024 * 1024;
        p++;
        break;
      case 'g':
      case 'G':
        buffer_size *= 1024 * 1024 * 1024;
        p++;
        break;
      case 'p':
      case 'P':
        buffer_size *= sysconf(_SC_PAGESIZE);
        p++;
        break;
      }
      if (*p == 'b' || *p == 'B') {
        p++;
      }
      if (*p != '\0') {
        fprintf(stderr, "invalid buffer size: %s\n", optarg);
        return EXIT_FAILURE;
      }
      break;
    case 'V':
      printf("%s (%s) %s\n", program_invocation_short_name, PACKAGE_NAME,
             PACKAGE_VERSION);
      return EXIT_SUCCESS;
    case 'h':
      printf("Usage: pbuf [OPTION]...\n");
      printf("Copy standard input to standard output.\n");
      printf("\n");
      printf("Options:\n");
      printf("  -b, --buffer-size=SIZE : set buffer size\n");
      printf("          SIZE is an integer and optional unit (k, m, g, p)\n");
      printf("            k = kibibyte (1024)\n");
      printf("            m = mebibyte (1048576)\n");
      printf("            g = gibibyte (1073741824)\n");
      printf("            p = pagesize (%ld)\n", sysconf(_SC_PAGESIZE));
      printf("            default is 1p\n");
      printf("  -V, --version : print version and exit\n");
      printf("  -h, --help    : print this usage and exit\n");
      printf("\n");
      return EXIT_SUCCESS;
    default:
      return EXIT_FAILURE;
    }
  }
  return pbuf(STDIN_FILENO, STDOUT_FILENO, buffer_size) == 0 ? EXIT_SUCCESS
                                                             : EXIT_FAILURE;
}