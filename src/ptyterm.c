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

// プログラム終了時のコールバック関数
static struct termios saved_termios;
static void save_termios(int fd)
{
  if (ioctl(fd, TCGETS, &saved_termios) == -1) {
    perror("ioctl(TCGETS)");
    exit(EXIT_FAILURE);
  }
}
static void restore_termios(int fd)
{
  if (ioctl(fd, TCSETS, &saved_termios) == -1) {
    perror("ioctl(TCSETS)");
    exit(EXIT_FAILURE);
  }
}

static void
restore_termios_handler()
{
  restore_termios(STDIN_FILENO);
}

static char *slave_name = NULL;
static int
open_slave()
{
  int fd;
  
  if ((fd = open(slave_name, O_RDWR)) == -1) {
    perror("open(pty_slave)");
    exit(EXIT_FAILURE);
  }
  return fd;
}

static void
copy_termios(int srcfd, int dstfd)
{
  struct termios termios;

  if (ioctl(srcfd, TCGETS, &termios) == -1) {
    perror("ioctl(TCGETS)");
    exit(EXIT_FAILURE);
  }
  if (ioctl(dstfd, TCSETS, &termios) == -1) {
    perror("ioctl(TCSETS)");
    exit(EXIT_FAILURE);
  }
}

static void
copy_winsize(int srcfd, int dstfd)
{
  struct winsize winsize;

  if (ioctl(srcfd, TIOCGWINSZ, &winsize) == -1)  {
    perror("ioctl(TIOCGWINSZ)");
    exit(EXIT_FAILURE);
  }
  if (ioctl(dstfd, TIOCSWINSZ, &winsize) == -1) {
    perror("ioctl(TIOCSWINSZ)");
    exit(EXIT_FAILURE);
  }
}

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
  slave_name = ptsname(fd);
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

static void
set_control_tty(int fd)
{
  if (ioctl(fd, TIOCSCTTY, 0) == -1) {
    perror("ioctl(TIOCSCTTY)");
    exit(EXIT_FAILURE);
  }
}

static void
set_foreground_pgid(int fd, pid_t pid)
{
  if (ioctl(fd, TIOCSPGRP, &pid) == -1) {
    perror("ioctl(TIOCSPGRP)");
    exit(EXIT_FAILURE);
  }
}

// サイズ変更検出時のコールバック関数
static int srcfd = -1;
static int dstfd = -1;
static void size_changed(int sig)
{
  int sfd;

  if ((sfd = open_slave()) == -1) return;
  copy_winsize(srcfd, sfd);
  close(sfd);
}

int main(int argc, char * const argv[])
{
  int mfd, sfd;
  pid_t pid;
  int itty;
  int argoffset;

  for (;;) {
    int c, optindex;
    static struct option longopts[] = {
      {"version", no_argument, NULL, 'V'},
      {"help",    no_argument, NULL, 'h'},
      {NULL,                0, NULL,   0}
    };

    c = getopt_long(argc, argv, "Vh", longopts, &optindex);
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
      printf("  %s [ENVNAME=ENVVALUE ..] [cmd [arg ..]]\n", argv[0]);
      printf("\n");
      printf("Options:\n");
      printf("  -V, --version : print version\n");
      printf("  -h, --help    : print this usage\n");
      printf("\n");
      exit(EXIT_SUCCESS);
    default:
      exit(EXIT_FAILURE);
    }
  }

  argoffset = optind;
  while (argoffset < argc && index(argv[argoffset], '='))
    putenv(argv[argoffset ++]);

  itty = isatty(STDIN_FILENO);

  if (itty) {
    save_termios(STDIN_FILENO);
  }

  mfd = open_pty();

  // スレーブ端末を開く
  sfd = open_slave();

  if (itty) {
    // termios, winsizeを親と同一にする
    copy_termios(STDIN_FILENO, sfd);
    copy_winsize(STDIN_FILENO, sfd);
  }

  switch (pid = fork()) {
  case -1:
    perror("fork");
    exit(EXIT_FAILURE);
  case 0:
    // マスタ端末は閉じる
    if (close(mfd) == -1) {
      perror("close");
      exit(EXIT_FAILURE);
    }
    // このプロセスをセッションリーダーにする
    be_session_leader();
    // このプロセスの制御端末を開いたスレーブ端末にする
    set_control_tty(sfd);
    // 自分のPIDをフォアグラウンドプロセスグループとする
    set_foreground_pgid(sfd, getpgrp());
    // 標準入力をスレーブ端末とする
    dup2(sfd, STDIN_FILENO);
    // 標準出力をスレーブ端末とする
    dup2(sfd, STDOUT_FILENO);
    // 標準エラー出力をスレーブ端末とする
    dup2(sfd, STDERR_FILENO);
    // スレーブ端末は閉じる
    if (sfd != STDIN_FILENO && sfd != STDOUT_FILENO && sfd != STDERR_FILENO) {
      close(sfd);
    }
    // 引数をコマンドとして実行する
    if (argc == argoffset) {
      char * const shell = getenv("SHELL");
      char * const argv_shell[] = { shell ?: "/bin/sh", NULL };
      
      execvp(argv_shell[0], argv_shell);
      perror(argv_shell[0]);
    } else {
      execvp(argv[argoffset], argv + argoffset);
      perror(argv[1]);
    }
    exit(EXIT_FAILURE);
  default:
    if (close(sfd) == -1) {
      perror("close");
      exit(EXIT_FAILURE);
    }
    if (itty) {
      struct termios termios;
      struct sigaction sigact;

      // 親端末のキーボードシグナルを無効化、端末エコーを無効化、カノニカルモードを無効化
      termios = saved_termios;
      termios.c_lflag &= ~ICANON & ~ECHO & ~ISIG;
      if (ioctl(STDIN_FILENO, TCSETS, &termios) == -1) {
        perror("ioctl(TCGETS)");
        exit(EXIT_FAILURE);
      }

      // プログラム終了時に元に戻す
      atexit(restore_termios_handler);
      srcfd = STDIN_FILENO;
      dstfd = mfd;
    
      sigact.sa_handler = size_changed;
      sigemptyset(&sigact.sa_mask);
      sigact.sa_flags = 0;

      if (sigaction(SIGWINCH, &sigact, NULL) == -1) {
        perror("sigaction(SWIGWINCH)");
        exit(EXIT_FAILURE);
      }
    }
    char buf1[4096], buf2[4096];
    size_t siz1 = 0, siz2 = 0;
    for (;;) {
      fd_set rfds, wfds;
      int maxfd, nfds;

      FD_ZERO(&rfds);
      FD_ZERO(&wfds);
      maxfd = -1;
      
      // 擬似端末 -> 標準出力 用のバッファに空きがあれば読出可能かチェック対象とする
      if (mfd >= 0 && siz1 < sizeof(buf1)) {
        FD_SET(mfd, &rfds);
        if (maxfd < mfd) maxfd = mfd;
      }
      // 標準入力 -> 擬似端末 用のバッファにデータがあれば書込可能かチェック対象とする
      if (mfd >= 0 && siz2 >            0) {
        FD_SET(mfd, &wfds);
        if (maxfd < mfd) maxfd = mfd;
      }
      // 標準入力 -> 擬似端末 用のバッファに空きがあれば読出可能かチェック対象とする
      if (siz2 < sizeof(buf2)) {
        FD_SET(STDIN_FILENO, &rfds);
        if (maxfd < STDIN_FILENO) maxfd = STDIN_FILENO;
      }
      // 擬似端末 -> 標準出力 用のバッファにデータがあれば書込可能かチェック対象とする
      if (siz1 >            0) {
        FD_SET(STDOUT_FILENO, &wfds);
        if (maxfd < STDOUT_FILENO) maxfd = STDOUT_FILENO;
      }

      // 読み書き状態をチェックする
      if ((nfds = select(maxfd + 1, &rfds, &wfds,  NULL, NULL)) == -1) {
        if (errno == EINTR) continue;
        perror("select");
        exit(EXIT_FAILURE);
      }

      // 擬似端末に書込
      if (FD_ISSET(mfd, &wfds)) {
        ssize_t siz = write(mfd, buf2, siz2);
        if (siz == -1) {
          perror("write");
          exit(EXIT_FAILURE);
        }
        if (siz < siz2) {
          memmove(buf2, buf2 + siz, siz2 - siz);
        }
        siz2 -= siz;
      }

      // 標準出力に書込
      if (FD_ISSET(STDOUT_FILENO, &wfds)) {
        ssize_t siz = write(STDOUT_FILENO, buf1, siz1);
        if (siz == -1) {
          perror("write");
          exit(EXIT_FAILURE);
        }
        if (siz < siz1) {
          memmove(buf1, buf1 + siz, siz1 - siz);
        }
        siz1 -= siz;
      }

      // 擬似端末から読出
      if (FD_ISSET(mfd, &rfds)) {
        ssize_t siz = read(mfd, buf1 + siz1, sizeof(buf1) - siz1);
        if (siz == -1) {
          if (errno == EIO) {
            int status;
            int ret_status = EXIT_SUCCESS;
            pid_t wpid;

            close(mfd);
            mfd = -1;
            for (;;) {
              if ((wpid = wait(&status)) == -1) {
                if (errno == ECHILD) {
                  exit(ret_status);
                }
                if (errno == EINTR) {
                  continue;
                }
                perror("waitpid");
                exit(EXIT_FAILURE);
              }
              if (wpid == pid) {
                ret_status = status;
              }
            }
          }
          perror("read");
          exit(EXIT_FAILURE);
        }
        if (siz == 0) {
          // TODO: EOF 後で考える
          exit(EXIT_SUCCESS);
        }
        siz1 += siz;
      }

      // 標準入力から読出
      if (FD_ISSET(STDIN_FILENO, &rfds)) {
        ssize_t siz = read(STDIN_FILENO, buf2 + siz2, sizeof(buf2) - siz2);
        if (siz == -1) {
          perror("read");
          exit(EXIT_FAILURE);
        }
        if (siz == 0) {
          // TODO: EOF 後で考える
          exit(EXIT_SUCCESS);
        }
        siz2 += siz;
      }

    }
  }
}
