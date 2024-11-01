#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char *const argv[]) {
  int fd, max_fd, fd_count;
  fd_set rfds, wfds;
  char ibuf[8192], obuf[8192];
  size_t isiz = 0, osiz = 0;
  struct stat st;
  fd_set ofds;
  char *filename = NULL;

  setlocale(LC_ALL, "");
  switch (argc) {
  case 1:
    filename = ttyname(STDIN_FILENO);
    if (filename == NULL) {
      perror("ttyname");
      exit(EXIT_FAILURE);
    }
    break;
  case 2:
    filename = argv[1];
    break;
  default:
    fprintf(stderr, "invalid argument\n");
    exit(EXIT_FAILURE);
  }
  fd = open(filename, O_RDWR | O_NOCTTY);
  if (fd == -1) {
    perror(filename);
    exit(EXIT_FAILURE);
  }
  if (fstat(fd, &st) == -1) {
    perror("fstat");
    exit(EXIT_FAILURE);
  }
  if (!S_ISCHR(st.st_mode) && !S_ISSOCK(st.st_mode)) {
    fprintf(stderr,
            "invalid input file (must be Character Device or Socket)\n");
    exit(EXIT_FAILURE);
  }
  FD_ZERO(&ofds);
  FD_SET(STDIN_FILENO, &ofds);
  FD_SET(STDOUT_FILENO, &ofds);
  FD_SET(fd, &ofds);

  for (;;) {
    max_fd = -1;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    if (isiz < sizeof(ibuf) && FD_ISSET(STDIN_FILENO, &ofds)) {
      FD_SET(STDIN_FILENO, &rfds);
      if (max_fd < STDIN_FILENO) {
        max_fd = STDIN_FILENO;
      }
    }
    if (osiz < sizeof(obuf) && FD_ISSET(fd, &ofds)) {
      FD_SET(fd, &rfds);
      if (max_fd < fd) {
        max_fd = fd;
      }
    }
    if (isiz > 0 && FD_ISSET(fd, &ofds)) {
      FD_SET(fd, &wfds);
      if (max_fd < fd) {
        max_fd = fd;
      }
    }
    if (osiz > 0 && FD_ISSET(STDOUT_FILENO, &ofds)) {
      FD_SET(STDOUT_FILENO, &wfds);
      if (max_fd < STDOUT_FILENO) {
        max_fd = STDOUT_FILENO;
      }
    }

    if (max_fd == -1) {
      exit(EXIT_SUCCESS);
    }

    fd_count = select(max_fd + 1, &rfds, &wfds, NULL, NULL);
    if (fd_count == -1) {
      if (errno == EINTR)
        continue;
      perror("select");
      exit(EXIT_FAILURE);
    }

    if (FD_ISSET(STDOUT_FILENO, &wfds)) {
      ssize_t siz = write(STDOUT_FILENO, obuf, osiz);
      if (siz == -1) {
        perror("write");
        exit(EXIT_FAILURE);
      }
      if (siz < osiz) {
        memmove(obuf, obuf + siz, osiz - siz);
      }
      osiz -= siz;
      continue;
    }
    if (FD_ISSET(fd, &wfds)) {
      ssize_t siz = write(fd, ibuf, isiz);
      if (siz == -1) {
        perror("write");
        exit(EXIT_FAILURE);
      }
      if (siz < isiz) {
        memmove(ibuf, ibuf + siz, isiz - siz);
      }
      isiz -= siz;
      continue;
    }
    if (FD_ISSET(STDIN_FILENO, &rfds)) {
      ssize_t siz = read(STDIN_FILENO, ibuf + isiz, sizeof(ibuf) - isiz);
      if (siz == -1) {
        perror("read");
        exit(EXIT_FAILURE);
      }
      if (siz == 0) {
        FD_CLR(STDIN_FILENO, &ofds);
      }
      isiz += siz;
      continue;
    }
    if (FD_ISSET(fd, &rfds)) {
      ssize_t siz = read(fd, obuf + osiz, sizeof(obuf) - osiz);
      if (siz == -1) {
        perror("read");
        exit(EXIT_FAILURE);
      }
      if (siz == 0) {
        FD_CLR(fd, &ofds);
      }
      osiz += siz;
      continue;
    }
  }
}
