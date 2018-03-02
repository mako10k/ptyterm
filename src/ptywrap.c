#define _GNU_SOURCE
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <pty.h>
#include <signal.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <sys/wait.h>
#include <getopt.h>

#include "config.h"

static int
open_pty() {
  int fd;

  if ((fd = open("/dev/ptmx", O_RDWR)) == -1) {
    perror("open(\"/dev/ptmx\")");
    exit(EXIT_FAILURE);
  }
  if (grantpt(fd) == -1) {
    perror("grantpt");
    exit(EXIT_FAILURE);
  }
  if (unlockpt(fd) == -1) {
    perror("unlockpt");
    exit(EXIT_FAILURE);
  }
  return fd;
}

static void
be_session_leader()
{
  if (setsid() == -1) {
    perror("setsid");
    exit(EXIT_FAILURE);
  }
}

int main(int argc, char * const argv[])
{
  int mfd;
  pid_t pid;
  int argoffset;
  int opt_daemon = 0;

  for (;;) {
    int c, optindex;
    static struct option longopts[] = {
      {"version", no_argument, NULL, 'V'},
      {"help",    no_argument, NULL, 'h'},
      {"daemon",  no_argument, NULL, 'd'},
      {NULL,                0, NULL,   0}
    };

    c = getopt_long(argc, argv, "Vhd", longopts, &optindex);
    if (c == -1) break;

    switch (c) {
    case 'h':
    case 'V':
      printf("%s\n", PACKAGE_STRING);
      if (c != 'h') exit(EXIT_SUCCESS);
      printf("\n");
      printf("Usage:\n");
      printf("  %s -V\n", argv[0]);
      printf("  %s -h\n", argv[0]);
      printf("  %s [-d] [ENVNAME=ENVVALUE ..] [cmd [arg ..]]\n", argv[0]);
      printf("\n");
      printf("Options:\n");
      printf("  -V, --version : print version\n");
      printf("  -h, --help    : print this usage\n");
      printf("  -d, --daemon  : daemon mode\n");
      printf("\n");
      exit(EXIT_SUCCESS);
    case 'd':
      opt_daemon = 1;
      break;
    default:
      exit(EXIT_FAILURE);
    }
  }

  argoffset = optind;
  while (argoffset < argc && index(argv[argoffset], '='))
    putenv(argv[argoffset ++]);

  mfd = open_pty();
  puts(ptsname(mfd));

  switch (pid = fork()) {
  case -1:
    perror("fork");
    exit(EXIT_FAILURE);
  case 0:
    // このプロセスをセッションリーダーにする
    be_session_leader();

    // 標準入力をマスタ端末とする
    dup2(mfd, STDIN_FILENO);
    // 標準出力をマスタ端末とする
    dup2(mfd, STDOUT_FILENO);
    // デーモンモードの場合は標準エラー出力を /dev/null にする
    if (opt_daemon) {
      int nfd;

      if ((nfd = open("/dev/null", O_WRONLY)) == -1) {
        perror("open(\"/dev/null\")");
        exit(EXIT_FAILURE);
      }
      dup2(nfd, STDERR_FILENO);
      if (nfd != STDERR_FILENO) {
        close(nfd);
      }
    }
    // マスタ端末は閉じる
    if (mfd != STDIN_FILENO && mfd != STDOUT_FILENO) {
      close(mfd);
    }
    // 引数をコマンドとして実行する
    if (argc == argoffset) {
      char * const argv_cat[] = { "/bin/cat" };
      
      execvp(argv_cat[0], argv_cat);
      perror(argv_cat[0]);
    } else {
      execvp(argv[argoffset], argv + argoffset);
      perror(argv[1]);
    }
    exit(EXIT_FAILURE);
  default:
    close(mfd);
    if (!opt_daemon) {
      for (;;) {
        int status;
        pid_t wpid;

        if ((wpid = wait(&status)) == -1) {
          if (errno == EINTR) continue;
          if (errno == ECHILD) {
            exit(status);
          }
          perror("wait");
          exit(EXIT_FAILURE);
        }
      }
    }
    exit(EXIT_SUCCESS);
  }
}
