#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define PACKAGE_STRING "ptyterm"
#endif

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/// @brief 端末の termios を保存場所
static struct termios saved_termios;

/// @brief 端末の termios を保存する
/// @param[in] fd 端末のファイルディスクリプタ
static void save_termios(int fd) {
  if (ioctl(fd, TCGETS, &saved_termios) == -1) {
    perror("ioctl(TCGETS)");
    exit(EXIT_FAILURE);
  }
}

/// @brief 端末の termios を復元する
/// @param[in] fd 端末のファイルディスクリプタ
static void restore_termios(int fd) {
  if (ioctl(fd, TCSETS, &saved_termios) == -1) {
    perror("ioctl(TCSETS)");
    exit(EXIT_FAILURE);
  }
}

/// @brief プログラム終了時のコールバック関数
static void restore_termios_handler() { restore_termios(STDIN_FILENO); }

/// @brief スレーブ端末名
static char *slave_name = NULL;

/// @brief スレーブ端末を開く
/// @return スレーブ端末のファイルディスクリプタ
static int open_slave() {
  int fd;

  if ((fd = open(slave_name, O_RDWR)) == -1) {
    perror("open(pty_slave)");
    exit(EXIT_FAILURE);
  }
  return fd;
}

#if 0
/// @brief termios をコピーする
/// @param[in] srcfd コピー元のファイルディスクリプタ
/// @param[in] dstfd コピー先のファイルディスクリプタ
static void
copy_termios (int srcfd, int dstfd)
{
  struct termios termios;

  if (ioctl (srcfd, TCGETS, &termios) == -1)
    {
      perror ("ioctl(TCGETS)");
      exit (EXIT_FAILURE);
    }
  if (ioctl (dstfd, TCSETS, &termios) == -1)
    {
      perror ("ioctl(TCSETS)");
      exit (EXIT_FAILURE);
    }
}
#endif

/// @brief カラム数
static int opt_cols = -1;

/// @brief カラム数
static int opt_lines = -1;

/// @brief winsize をコピーする
/// @param[in] srcfd コピー元のファイルディスクリプタ
/// @param[in] dstfd コピー先のファイルディスクリプタ
/// @param[in] cols カラム数 (-1 ならコピーしない)
/// @param[in] lines ライン数 (-1 ならコピーしない)
static void copy_winsize(int srcfd, int dstfd) {
  struct winsize winsize;

  if (ioctl(srcfd, TIOCGWINSZ, &winsize) == -1) {
    perror("ioctl(TIOCGWINSZ)");
    exit(EXIT_FAILURE);
  }
  if (opt_cols > 0) {
    winsize.ws_col = opt_cols;
  }
  if (opt_lines > 0) {
    winsize.ws_row = opt_lines;
  }
  if (ioctl(dstfd, TIOCSWINSZ, &winsize) == -1) {
    perror("ioctl(TIOCSWINSZ)");
    exit(EXIT_FAILURE);
  }
}

/// @brief 擬似端末を開く
/// @return 擬似端末のファイルディスクリプタ
static int open_pty() {
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

/// @brief セッションリーダーになる
static void be_session_leader() {
  if (setsid() == -1) {
    perror("setsid");
    exit(EXIT_FAILURE);
  }
}

/// @brief 入力のファイルディスクリプタ
static int srcfd = -1;

/// @brief 出力のファイルディスクプリ他
static int dstfd = -1;

/// @brief ウィンドウサイズ変更時のハンドラ
/// @param[in] sig シグナル番号
static void size_changed(int sig) {
  int sfd;

  if ((sfd = open_slave()) == -1)
    return;
  copy_winsize(srcfd, sfd);
  close(sfd);
}

/// @brief メイン関数
/// @param[in] argc 引数の数
/// @param[in] argv 引数の配列
int main(int argc, char *const argv[]) {
  /// @brief マスタ端末のファイルディスクリプタ
  int mfd;
  /// @brief スレーブ端末のファイルディスクリプタ
  int sfd;
  /// @brief プロセスID
  pid_t pid;
  /// @brief 標準入力が端末かどうか
  int itty;
  /// @brief 引数のオフセット
  int argoffset;

  setlocale(LC_ALL, "");
  while (1) {
    /// @brief オプション
    int opt;
    /// @brief オプションのインデックス
    int optindex;

    char *p;
    /// @brief オプションの定義
    static struct option longopts[] = {{"version", no_argument, NULL, 'V'},
                                       {"help", no_argument, NULL, 'h'},
                                       {"cols", required_argument, NULL, 'c'},
                                       {"lines", required_argument, NULL, 'l'},
                                       {NULL, 0, NULL, 0}};

    /// @brief オプションを取得する
    opt = getopt_long(argc, argv, "Vhc:l:", longopts, &optindex);
    if (opt == -1)
      break;

    switch (opt) {
    case 'h':
    case 'V':
      printf("%s\n", PACKAGE_STRING);
      if (opt != 'h')
        exit(EXIT_SUCCESS);
      printf("\n");
      printf("Usage:\n");
      printf("  %s [options] [ENVNAME=ENVVALUE ..] [cmd [arg ..]]\n", argv[0]);
      printf("\n");
      printf("Options:\n");
      printf("  -c, --cols=N  : set columns\n");
      printf("  -l, --lines=N : set lines\n");
      printf("  -V, --version : print version and exit\n");
      printf("  -h, --help    : print this usage and exit\n");
      printf("\n");
      exit(EXIT_SUCCESS);
    case 'c':
      opt_cols = strtol(optarg, &p, 0);
      if (optarg == p || *p != '\0' || opt_cols <= 0) {
        fprintf(stderr, "invalid cols: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;
    case 'l':
      opt_lines = strtol(optarg, &p, 0);
      if (optarg == p || *p != '\0' || opt_lines <= 0) {
        fprintf(stderr, "invalid lines: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;
    default:
      exit(EXIT_FAILURE);
    }
  }

  /// @brief 環境変数を設定する
  argoffset = optind;
  /// @brief 環境変数を設定する
  while (argoffset < argc && index(argv[argoffset], '='))
    putenv(argv[argoffset++]);

  /// @brief 標準入力が端末かどうか
  itty = isatty(STDIN_FILENO);
  if (itty) {
    save_termios(STDIN_FILENO);
  }

  // 擬似端末を開く
  mfd = open_pty();

  switch (pid = fork()) {
  case -1:
    /// @brief プロセスのフォークに失敗した場合
    perror("fork");
    exit(EXIT_FAILURE);
  case 0:
    /// @brief 子プロセスの場合
    // マスタ端末は閉じる
    if (close(mfd) == -1) {
      perror("close");
      exit(EXIT_FAILURE);
    }

    // このプロセスをセッションリーダーにする
    be_session_leader();

    // スレーブ端末を開く
    sfd = open_slave();

    if (itty) {
      // termios, winsizeを親と同一にする
      restore_termios(sfd);
      copy_winsize(STDIN_FILENO, sfd);
    } else if (opt_cols > 0 || opt_lines > 0) {
      if (opt_cols <= 0) {
        fprintf(stderr, "cols is not set with lines for notty inputs\n");
        exit(EXIT_FAILURE);
      }
      if (opt_lines <= 0) {
        fprintf(stderr, "lines is not set with cols for notty inputs\n");
        exit(EXIT_FAILURE);
      }
      struct winsize winsize = {opt_lines, opt_cols, 0, 0};
      if (ioctl(sfd, TIOCSWINSZ, &winsize) == -1) {
        perror("ioctl(TIOCSWINSZ)");
        exit(EXIT_FAILURE);
      }
    }

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
      char *const shell = getenv("SHELL");
      char *const argv_shell[] = {shell ?: "/bin/sh", NULL};

      execvp(argv_shell[0], argv_shell);
      perror(argv_shell[0]);
    } else {
      execvp(argv[argoffset], argv + argoffset);
      perror(argv[argoffset]);
    }
    exit(EXIT_FAILURE);
  default:
    if (itty) {
      struct termios termios;
      struct sigaction sigact;

      // 親端末のキーボードシグナルを無効化、端末エコーを無効化、カノニカルモードを無効化
      termios = saved_termios;
      termios.c_lflag &= ~ICANON & ~ECHO & ~ISIG & ~IEXTEN;
      termios.c_iflag &= ~BRKINT & ~ICRNL & ~INPCK & ~ISTRIP & ~IXON;
      termios.c_cflag &= ~CSIZE & ~PARENB;
      termios.c_cflag |= CS8;
      termios.c_cc[VMIN] = 1;
      termios.c_cc[VTIME] = 0;

      if (ioctl(STDIN_FILENO, TCSETSF, &termios) == -1) {
        perror("ioctl(TCSETSF)");
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

      // 擬似端末 -> 標準出力
      // 用のバッファに空きがあれば読出可能かチェック対象とする
      if (mfd >= 0 && siz1 < sizeof(buf1)) {
        FD_SET(mfd, &rfds);
        if (maxfd < mfd)
          maxfd = mfd;
      }
      // 標準入力 -> 擬似端末
      // 用のバッファにデータがあれば書込可能かチェック対象とする
      if (mfd >= 0 && siz2 > 0) {
        FD_SET(mfd, &wfds);
        if (maxfd < mfd)
          maxfd = mfd;
      }
      // 標準入力 -> 擬似端末
      // 用のバッファに空きがあれば読出可能かチェック対象とする
      if (siz2 < sizeof(buf2)) {
        FD_SET(STDIN_FILENO, &rfds);
        if (maxfd < STDIN_FILENO)
          maxfd = STDIN_FILENO;
      }
      // 擬似端末 -> 標準出力
      // 用のバッファにデータがあれば書込可能かチェック対象とする
      if (siz1 > 0) {
        FD_SET(STDOUT_FILENO, &wfds);
        if (maxfd < STDOUT_FILENO)
          maxfd = STDOUT_FILENO;
      }

      // 読み書き状態をチェックする
      if ((nfds = select(maxfd + 1, &rfds, &wfds, NULL, NULL)) == -1) {
        if (errno == EINTR)
          continue;
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
