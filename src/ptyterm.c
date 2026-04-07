#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define PACKAGE_STRING "ptyterm"
#endif

#include "ptyterm-control.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <ctype.h>
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
#include <time.h>
#include <unistd.h>

static struct termios saved_termios;
static int g_ifd = -1;
static volatile sig_atomic_t attach_resize_requested = 0;
static const char *g_program_path = "ptyterm";

enum ptyterm_status_format {
  PTYTERM_STATUS_FORMAT_TEXT = 1,
  PTYTERM_STATUS_FORMAT_KV = 2,
};

enum ptyterm_help_format {
  PTYTERM_HELP_FORMAT_TEXT = 1,
  PTYTERM_HELP_FORMAT_YAML = 2,
};

static int set_nonblocking(int fd);
static int resolve_socket_path(const char *socket_path,
                               char *default_socket_path,
                               const char **resolved_socket_path);
static int connect_daemon_socket(const char *socket_path,
                                 char *default_socket_path,
                                 int auto_start);
static void save_termios(int fd);
static void restore_termios_handler(void);
static void attach_size_changed(int sig);
static int parse_status_format(const char *value);
static int parse_help_format(const char *value);
static int parse_duration_ms(const char *value, uint64_t *duration_ms_out);
static void print_help(FILE *out, const char *program_name, int help_format);
static int usage_error(const char *program_name, const char *fmt, ...);
static void print_create_status(const struct ptyterm_create_response *response,
                int status_format);
static void print_buffer_info_status(
  const struct ptyterm_buffer_info_response *response, int status_format);
static void print_detach_status(const struct ptyterm_detach_response *response,
                int status_format);
static void print_resize_status(const struct ptyterm_resize_response *response,
                int status_format);
static void print_daemon_status(int running, int daemon_pid,
                int status_format);
static void print_daemon_stop_status(int stopping, int daemon_pid,
                   int status_format);
static void print_recv_status_line(uint32_t returned_bytes, uint64_t start_offset,
                                   uint64_t end_offset, uint64_t next_recv_offset,
                                   int truncated, const char *reason);

static int set_nonblocking(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL);
  if (flags == -1)
    return -1;
  if ((flags & O_NONBLOCK) != 0)
    return 0;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int parse_status_format(const char *value) {
  if (strcmp(value, "text") == 0)
    return PTYTERM_STATUS_FORMAT_TEXT;
  if (strcmp(value, "kv") == 0)
    return PTYTERM_STATUS_FORMAT_KV;
  return -1;
}

static int parse_help_format(const char *value) {
  if (strcmp(value, "text") == 0)
    return PTYTERM_HELP_FORMAT_TEXT;
  if (strcmp(value, "yaml") == 0)
    return PTYTERM_HELP_FORMAT_YAML;
  return -1;
}

static int parse_duration_ms(const char *value, uint64_t *duration_ms_out) {
  char *end;
  unsigned long long number;
  uint64_t multiplier;

  errno = 0;
  number = strtoull(value, &end, 0);
  if (errno != 0 || value == end || number == 0)
    return -1;

  if (strcmp(end, "ms") == 0)
    multiplier = 1;
  else if (strcmp(end, "s") == 0)
    multiplier = 1000;
  else
    return -1;

  if (number > UINT64_MAX / multiplier)
    return -1;

  *duration_ms_out = (uint64_t)number * multiplier;
  return 0;
}

static uint64_t monotonic_time_ms(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
    return 0;
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static const char *find_bytes(const char *haystack, size_t haystack_size,
                              const char *needle, size_t needle_size) {
  size_t offset;

  if (needle_size == 0)
    return haystack;
  if (needle_size > haystack_size)
    return NULL;

  for (offset = 0; offset + needle_size <= haystack_size; ++offset) {
    if (memcmp(haystack + offset, needle, needle_size) == 0)
      return haystack + offset;
  }
  return NULL;
}

static void print_recv_status_line(uint32_t returned_bytes, uint64_t start_offset,
                                   uint64_t end_offset, uint64_t next_recv_offset,
                                   int truncated, const char *reason) {
  fprintf(stderr,
          "recv %u bytes; offsets %llu..%llu; next-offset=%llu; truncated=%s; reason=%s\n",
          returned_bytes, (unsigned long long)start_offset,
          (unsigned long long)end_offset,
          (unsigned long long)next_recv_offset,
          truncated ? "yes" : "no", reason);
}

static void print_help_discovery(FILE *out, const char *program_name) {
  fprintf(out, "Structured help:\n");
  fprintf(out, "  %s --help-format=yaml\n", program_name);
  fprintf(out, "\n");
}

static void print_text_help(FILE *out, const char *program_name) {
  fprintf(out, "%s\n", PACKAGE_STRING);
  fprintf(out, "\n");
  fprintf(out, "Usage:\n");
  fprintf(out, "  %s [options] [ENVNAME=ENVVALUE ..] [cmd [arg ..]]\n",
          program_name);
  fprintf(out, "\n");
  fprintf(out, "Run options:\n");
  fprintf(out, "  -c, --cols=N  : set columns\n");
  fprintf(out, "  -l, --lines=N : set lines\n");
  fprintf(out,
          "  -i, --stdin=FILE : read from FILE instead of stdin (alias: --input)\n");
  fprintf(out,
          "  -o, --stdout=FILE : write to FILE instead of stdout (alias: --output)\n");
  fprintf(out,
          "  -a, --stdout-append=FILE : append output to FILE instead of stdout (alias: --append)\n");
  fprintf(out, "\n");
  fprintf(out, "Management options:\n");
  fprintf(out, "  -A, --attach        : attach to one daemon-managed session\n");
  fprintf(out, "  -C, --create        : create a daemon-managed session\n");
  fprintf(out, "      --daemon-status : report whether the daemon is running\n");
  fprintf(out, "      --daemon-stop   : request graceful daemon shutdown\n");
  fprintf(out, "      --status-format=text|kv : select structured status output\n");
  fprintf(out, "  -D, --detach        : detach one attached daemon-managed session\n");
  fprintf(out, "  -R, --resize        : resize one daemon-managed session\n");
  fprintf(out, "  -L, --list          : list daemon-managed sessions\n");
  fprintf(out, "  -B, --buffer-info   : show buffer state for one session\n");
  fprintf(out, "      --send=DATA     : send decoded bytes to one session\n");
  fprintf(out, "      --recv          : receive buffered output from one session\n");
  fprintf(out, "      --recv-size=N   : maximum bytes returned by --recv\n");
  fprintf(out, "      --recv-timeout=DURATION : wait up to DURATION (ms|s) for recv\n");
  fprintf(out, "      --recv-until=STRING : wait until unread output contains STRING\n");
  fprintf(out, "      --peek          : inspect buffered output without advancing recv\n");
  fprintf(out, "      --session=ID    : select one session for management operations\n");
  fprintf(out, "      --rows=N        : rows for --resize (alias: --lines)\n");
  fprintf(out, "      --cols=N        : cols for --resize\n");
  fprintf(out, "  -s, --socket=PATH   : override daemon control socket path\n");
  fprintf(out, "\n");
  fprintf(out, "Common options:\n");
  fprintf(out, "  -V, --version             : print version and exit\n");
  fprintf(out, "  -h, --help                : print this usage and exit\n");
  fprintf(out, "      --help-format=text|yaml : print help in the selected format\n");
  fprintf(out, "\n");
  print_help_discovery(out, program_name);
}

static void print_yaml_help(FILE *out, const char *program_name) {
  fprintf(out, "program: ptyterm\n");
  fprintf(out, "version: \"%s\"\n", PACKAGE_VERSION);
  fprintf(out, "summary: Interactive PTY wrapper and daemon session client\n");
  fprintf(out, "default_mode: run\n");
  fprintf(out, "discovery:\n");
  fprintf(out, "  text_help: \"%s --help\"\n", program_name);
  fprintf(out, "  structured_help: \"%s --help-format=yaml\"\n",
          program_name);
  fprintf(out, "usage:\n");
  fprintf(out, "  - \"%s [options] [ENVNAME=ENVVALUE ..] [cmd [arg ..]]\"\n",
          program_name);
  fprintf(out, "sections:\n");
  fprintf(out, "  common_options:\n");
  fprintf(out, "    - long: --help\n");
  fprintf(out, "      short: -h\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      description: Print text help and exit.\n");
  fprintf(out, "    - long: --help-format\n");
  fprintf(out, "      short: null\n");
  fprintf(out, "      argument: text|yaml\n");
  fprintf(out, "      default: text\n");
  fprintf(out, "      description: Print help in the selected format and exit.\n");
  fprintf(out, "    - long: --version\n");
  fprintf(out, "      short: -V\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      description: Print version and exit.\n");
  fprintf(out, "    - long: --socket\n");
  fprintf(out, "      short: -s\n");
  fprintf(out, "      argument: PATH\n");
  fprintf(out, "      description: Override daemon control socket path.\n");
  fprintf(out, "  run_options:\n");
  fprintf(out, "    - long: --stdin\n");
  fprintf(out, "      short: -i\n");
  fprintf(out, "      aliases: [--input]\n");
  fprintf(out, "      argument: FILE\n");
  fprintf(out, "      description: Read from FILE instead of stdin.\n");
  fprintf(out, "    - long: --stdout\n");
  fprintf(out, "      short: -o\n");
  fprintf(out, "      aliases: [--output]\n");
  fprintf(out, "      argument: FILE\n");
  fprintf(out, "      description: Write to FILE instead of stdout.\n");
  fprintf(out, "    - long: --stdout-append\n");
  fprintf(out, "      short: -a\n");
  fprintf(out, "      aliases: [--append]\n");
  fprintf(out, "      argument: FILE\n");
  fprintf(out, "      description: Append output to FILE instead of stdout.\n");
  fprintf(out, "    - long: --cols\n");
  fprintf(out, "      short: -c\n");
  fprintf(out, "      argument: N\n");
  fprintf(out, "      description: Set initial PTY columns.\n");
  fprintf(out, "    - long: --lines\n");
  fprintf(out, "      short: -l\n");
  fprintf(out, "      aliases: [--rows]\n");
  fprintf(out, "      argument: N\n");
  fprintf(out, "      description: Set initial PTY lines.\n");
  fprintf(out, "  management_operations:\n");
  fprintf(out, "    - long: --attach\n");
  fprintf(out, "      short: -A\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      requires: [--session]\n");
  fprintf(out, "      description: Attach to one daemon-managed session.\n");
  fprintf(out, "    - long: --create\n");
  fprintf(out, "      short: -C\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      description: Create a daemon-managed session.\n");
  fprintf(out, "    - long: --daemon-status\n");
  fprintf(out, "      short: null\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      description: Report whether the daemon is running.\n");
  fprintf(out, "    - long: --daemon-stop\n");
  fprintf(out, "      short: null\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      description: Request graceful daemon shutdown.\n");
  fprintf(out, "    - long: --detach\n");
  fprintf(out, "      short: -D\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      requires: [--session]\n");
  fprintf(out, "      description: Detach one attached daemon-managed session.\n");
  fprintf(out, "    - long: --resize\n");
  fprintf(out, "      short: -R\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      requires: [--session, --rows, --cols]\n");
  fprintf(out, "      description: Resize one daemon-managed session.\n");
  fprintf(out, "    - long: --list\n");
  fprintf(out, "      short: -L\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      description: List daemon-managed sessions.\n");
  fprintf(out, "    - long: --buffer-info\n");
  fprintf(out, "      short: -B\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      requires: [--session]\n");
  fprintf(out, "      description: Show buffer state for one session.\n");
  fprintf(out, "    - long: --send\n");
  fprintf(out, "      short: null\n");
  fprintf(out, "      argument: DATA\n");
  fprintf(out, "      requires: [--session]\n");
  fprintf(out, "      description: Send decoded bytes to one session.\n");
  fprintf(out, "    - long: --recv\n");
  fprintf(out, "      short: null\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      requires: [--session]\n");
  fprintf(out, "      description: Receive buffered output from one session.\n");
  fprintf(out, "    - long: --peek\n");
  fprintf(out, "      short: null\n");
  fprintf(out, "      argument: none\n");
  fprintf(out, "      requires: [--recv]\n");
  fprintf(out, "      description: Read buffered output without advancing the recv cursor.\n");
  fprintf(out, "    - long: --recv-size\n");
  fprintf(out, "      short: null\n");
  fprintf(out, "      argument: N\n");
  fprintf(out, "      requires: [--recv]\n");
  fprintf(out, "      default: 4096\n");
  fprintf(out, "      description: Maximum bytes returned by recv.\n");
  fprintf(out, "    - long: --recv-timeout\n");
  fprintf(out, "      short: null\n");
  fprintf(out, "      argument: DURATION\n");
  fprintf(out, "      requires: [--recv]\n");
  fprintf(out, "      description: Wait up to the requested duration for recv data or a pattern match.\n");
  fprintf(out, "    - long: --recv-until\n");
  fprintf(out, "      short: null\n");
  fprintf(out, "      argument: STRING\n");
  fprintf(out, "      requires: [--recv]\n");
  fprintf(out, "      description: Return once unread output contains the fixed string.\n");
  fprintf(out, "    - long: --session\n");
  fprintf(out, "      short: -S\n");
  fprintf(out, "      argument: ID\n");
  fprintf(out, "      description: Select one session for management operations.\n");
  fprintf(out, "examples:\n");
  fprintf(out, "  - description: Print text help\n");
  fprintf(out, "    command: \"%s --help\"\n", program_name);
  fprintf(out, "  - description: Print structured help\n");
  fprintf(out, "    command: \"%s --help-format=yaml\"\n", program_name);
  fprintf(out, "  - description: Create a managed session\n");
  fprintf(out, "    command: \"%s --create -- /bin/sh -c 'echo hello; sleep 30'\"\n",
          program_name);
  fprintf(out, "notes:\n");
  fprintf(out, "  - Exactly one management operation may be selected per invocation.\n");
  fprintf(out, "  - Management operations cannot be combined with standalone run output redirection except attach.\n");
  fprintf(out, "  - Status output formatting is controlled separately with --status-format=text|kv.\n");
}

static void print_help(FILE *out, const char *program_name, int help_format) {
  if (help_format == PTYTERM_HELP_FORMAT_YAML) {
    print_yaml_help(out, program_name);
    return;
  }

  print_text_help(out, program_name);
}

static int usage_error(const char *program_name, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  fprintf(stderr, "See '%s --help' for text help.\n", program_name);
  fprintf(stderr, "See '%s --help-format=yaml' for structured help.\n",
          program_name);
  return EXIT_FAILURE;
}

static void print_create_status(const struct ptyterm_create_response *response,
                                int status_format) {
  if (status_format == PTYTERM_STATUS_FORMAT_TEXT) {
    printf("session id: %u\n", response->session_id);
    printf("state: %s\n", ptyterm_session_state_name(response->state));
    printf("child pid: %d\n", response->child_pid);
    return;
  }

  printf("session_id=%u\n", response->session_id);
  printf("state=%s\n", ptyterm_session_state_name(response->state));
  printf("child_pid=%d\n", response->child_pid);
}

static void print_buffer_info_status(
    const struct ptyterm_buffer_info_response *response, int status_format) {
  if (status_format == PTYTERM_STATUS_FORMAT_TEXT) {
    printf("id: %u\n", response->id);
    printf("state: %s\n", ptyterm_session_state_name(response->state));
    printf("buffer capacity: %u\n", response->buffer_capacity);
    printf("buffer used: %u\n", response->buffer_used);
    printf("dropped bytes: %u\n", response->dropped_bytes);
    printf("paused on full: %s\n", response->paused_on_full ? "yes" : "no");
    return;
  }

  printf("id=%u\n", response->id);
  printf("state=%s\n", ptyterm_session_state_name(response->state));
  printf("buffer_capacity=%u\n", response->buffer_capacity);
  printf("buffer_used=%u\n", response->buffer_used);
  printf("dropped_bytes=%u\n", response->dropped_bytes);
  printf("paused_on_full=%u\n", response->paused_on_full);
}

static void print_detach_status(const struct ptyterm_detach_response *response,
                                int status_format) {
  if (status_format == PTYTERM_STATUS_FORMAT_TEXT) {
    printf("session id: %u\n", response->session_id);
    printf("state: %s\n", ptyterm_session_state_name(response->state));
    return;
  }

  printf("session_id=%u\n", response->session_id);
  printf("state=%s\n", ptyterm_session_state_name(response->state));
}

static void print_resize_status(const struct ptyterm_resize_response *response,
                                int status_format) {
  if (status_format == PTYTERM_STATUS_FORMAT_TEXT) {
    printf("session id: %u\n", response->session_id);
    printf("rows: %u\n", response->rows);
    printf("cols: %u\n", response->cols);
    return;
  }

  printf("session_id=%u\n", response->session_id);
  printf("rows=%u\n", response->rows);
  printf("cols=%u\n", response->cols);
}

static void print_daemon_status(int running, int daemon_pid,
                                int status_format) {
  if (status_format == PTYTERM_STATUS_FORMAT_TEXT) {
    printf("daemon: %s\n", running ? "running" : "stopped");
    printf("daemon pid: %d\n", daemon_pid);
    return;
  }

  printf("running=%s\n", running ? "yes" : "no");
  printf("daemon_pid=%d\n", daemon_pid);
}

static void print_daemon_stop_status(int stopping, int daemon_pid,
                                     int status_format) {
  if (status_format == PTYTERM_STATUS_FORMAT_TEXT) {
    printf("daemon: %s\n", stopping ? "stopping" : "stopped");
    printf("daemon pid: %d\n", daemon_pid);
    return;
  }

  printf("stopping=%s\n", stopping ? "yes" : "no");
  printf("daemon_pid=%d\n", daemon_pid);
}

static int resolve_socket_path(const char *socket_path,
                               char *default_socket_path,
                               const char **resolved_socket_path) {
  if (socket_path == NULL) {
    if (ptyterm_default_socket_path(default_socket_path,
                                    PTYTERM_SOCKET_PATH_MAX) == -1) {
      return -1;
    }
    socket_path = default_socket_path;
  }

  *resolved_socket_path = socket_path;
  return 0;
}

static int can_autostart_daemon(int errnum) {
  return errnum == ENOENT || errnum == ECONNREFUSED;
}

static int wait_for_daemon_socket(const char *socket_path) {
  int attempt;

  for (attempt = 0; attempt < 50; ++attempt) {
    int fd;

    fd = ptyterm_connect_socket(socket_path);
    if (fd != -1)
      return fd;
    if (errno != ENOENT && errno != ECONNREFUSED)
      return -1;
    usleep(100000);
  }

  errno = ETIMEDOUT;
  return -1;
}

static int start_daemon_process(const char *socket_path) {
  pid_t pid;

  pid = fork();
  if (pid == -1)
    return -1;
  if (pid == 0) {
    const char *daemon_argv[4];
    char daemon_path[PATH_MAX];
    const char *daemon_program;
    const char *slash;
    int devnull;

    if (setsid() == -1)
      _exit(127);

    devnull = open("/dev/null", O_RDWR);
    if (devnull == -1)
      _exit(127);
    if (dup2(devnull, STDIN_FILENO) == -1 ||
        dup2(devnull, STDOUT_FILENO) == -1 ||
        dup2(devnull, STDERR_FILENO) == -1) {
      _exit(127);
    }
    if (devnull > STDERR_FILENO)
      close(devnull);

    daemon_program = "ptytermd";
    slash = strrchr(g_program_path, '/');
    if (slash != NULL) {
      size_t prefix_size;

      prefix_size = (size_t)(slash - g_program_path + 1);
      if (prefix_size + strlen("ptytermd") + 1 > sizeof(daemon_path))
        _exit(127);
      memcpy(daemon_path, g_program_path, prefix_size);
      strcpy(daemon_path + prefix_size, "ptytermd");
      daemon_program = daemon_path;
    }

    daemon_argv[0] = daemon_program;
    daemon_argv[1] = "--socket";
    daemon_argv[2] = socket_path;
    daemon_argv[3] = NULL;

    if (slash != NULL)
      execv(daemon_program, (char *const *)daemon_argv);
    execvp(daemon_program, (char *const *)daemon_argv);
    _exit(127);
  }

  return wait_for_daemon_socket(socket_path);
}

static int connect_daemon_socket(const char *socket_path,
                                 char *default_socket_path,
                                 int auto_start) {
  const char *resolved_socket_path;
  int fd;

  if (resolve_socket_path(socket_path, default_socket_path,
                          &resolved_socket_path) == -1) {
    return -1;
  }

  fd = ptyterm_connect_socket(resolved_socket_path);
  if (fd != -1)
    return fd;
  if (!auto_start || !can_autostart_daemon(errno))
    return -1;

  return start_daemon_process(resolved_socket_path);
}

static int print_list_response(const void *payload, size_t payload_size) {
  const struct ptyterm_list_response *response;
  const struct ptyterm_session_summary *summary;
  size_t expected_size;
  uint32_t i;

  if (payload_size < sizeof(*response)) {
    fprintf(stderr, "invalid list response\n");
    return EXIT_FAILURE;
  }

  response = payload;
  expected_size = sizeof(*response) +
                  response->session_count * sizeof(struct ptyterm_session_summary);
  if (payload_size != expected_size) {
    fprintf(stderr, "invalid list response size\n");
    return EXIT_FAILURE;
  }

  if (response->session_count == 0) {
    printf("no sessions\n");
    return EXIT_SUCCESS;
  }

  summary = (const struct ptyterm_session_summary *)(response + 1);
  for (i = 0; i < response->session_count; ++i) {
    printf("%u\t%s\t%d\t%s\n", summary[i].id,
           ptyterm_session_state_name(summary[i].state), summary[i].child_pid,
           summary[i].command);
  }
  return EXIT_SUCCESS;
}

static int run_list_client(const char *socket_path, int session_id) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  char payload[4096];
  struct ptyterm_list_request request;
  struct ptyterm_message_header header;
  ssize_t payload_size;
  int fd;

  fd = connect_daemon_socket(socket_path, default_socket_path, 1);
  if (fd == -1) {
    perror(socket_path);
    return EXIT_FAILURE;
  }

  request.session_id = session_id;
  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_LIST_REQUEST, &request,
                           sizeof(request)) == -1) {
    perror("send");
    close(fd);
    return EXIT_FAILURE;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    perror("recv");
    close(fd);
    return EXIT_FAILURE;
  }

  close(fd);
  switch (header.type) {
  case PTYTERM_MESSAGE_LIST_RESPONSE:
    return print_list_response(payload, (size_t)payload_size);
  case PTYTERM_MESSAGE_ERROR: {
    const struct ptyterm_error_response *response;

    if ((size_t)payload_size < sizeof(*response)) {
      fprintf(stderr, "short error response\n");
      return EXIT_FAILURE;
    }
    response = (const struct ptyterm_error_response *)payload;
    fprintf(stderr, "%s\n", response->message);
    return EXIT_FAILURE;
  }
  default:
    fprintf(stderr, "unexpected response type: %u\n", header.type);
    return EXIT_FAILURE;
  }
}

static int run_buffer_info_client(const char *socket_path, int session_id,
                                  int status_format) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  char payload[4096];
  struct ptyterm_buffer_info_request request;
  struct ptyterm_message_header header;
  ssize_t payload_size;
  int fd;

  fd = connect_daemon_socket(socket_path, default_socket_path, 1);
  if (fd == -1) {
    perror(socket_path);
    return EXIT_FAILURE;
  }

  request.session_id = session_id;
  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_BUFFER_INFO_REQUEST, &request,
                           sizeof(request)) == -1) {
    perror("send");
    close(fd);
    return EXIT_FAILURE;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    perror("recv");
    close(fd);
    return EXIT_FAILURE;
  }

  close(fd);
  switch (header.type) {
  case PTYTERM_MESSAGE_BUFFER_INFO_RESPONSE: {
    const struct ptyterm_buffer_info_response *response;

    if ((size_t)payload_size != sizeof(*response)) {
      fprintf(stderr, "invalid buffer-info response size\n");
      return EXIT_FAILURE;
    }

    response = (const struct ptyterm_buffer_info_response *)payload;
    print_buffer_info_status(response, status_format);
    return EXIT_SUCCESS;
  }
  case PTYTERM_MESSAGE_ERROR: {
    const struct ptyterm_error_response *response;

    if ((size_t)payload_size < sizeof(*response)) {
      fprintf(stderr, "short error response\n");
      return EXIT_FAILURE;
    }
    response = (const struct ptyterm_error_response *)payload;
    fprintf(stderr, "%s\n", response->message);
    return EXIT_FAILURE;
  }
  default:
    fprintf(stderr, "unexpected response type: %u\n", header.type);
    return EXIT_FAILURE;
  }
}

static int run_create_client(const char *socket_path, int cmd_argc,
                             char *const cmd_argv[], int status_format) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  char payload[4096];
  struct ptyterm_create_request *request;
  struct ptyterm_message_header header;
  ssize_t payload_size;
  size_t offset;
  int fd;
  int i;

  request = (struct ptyterm_create_request *)payload;
  request->argc = (uint32_t)cmd_argc;
  offset = sizeof(*request);
  for (i = 0; i < cmd_argc; ++i) {
    size_t arg_size;

    arg_size = strlen(cmd_argv[i]) + 1;
    if (offset + arg_size > sizeof(payload)) {
      fprintf(stderr, "create request too large\n");
      return EXIT_FAILURE;
    }
    memcpy(payload + offset, cmd_argv[i], arg_size);
    offset += arg_size;
  }

  fd = connect_daemon_socket(socket_path, default_socket_path, 1);
  if (fd == -1) {
    perror(socket_path);
    return EXIT_FAILURE;
  }

  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_CREATE_REQUEST, payload,
                           (uint32_t)offset) == -1) {
    perror("send");
    close(fd);
    return EXIT_FAILURE;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    perror("recv");
    close(fd);
    return EXIT_FAILURE;
  }

  close(fd);
  switch (header.type) {
  case PTYTERM_MESSAGE_CREATE_RESPONSE: {
    const struct ptyterm_create_response *response;

    if ((size_t)payload_size != sizeof(*response)) {
      fprintf(stderr, "invalid create response size\n");
      return EXIT_FAILURE;
    }
    response = (const struct ptyterm_create_response *)payload;
    print_create_status(response, status_format);
    return EXIT_SUCCESS;
  }
  case PTYTERM_MESSAGE_ERROR: {
    const struct ptyterm_error_response *response;

    if ((size_t)payload_size < sizeof(*response)) {
      fprintf(stderr, "short error response\n");
      return EXIT_FAILURE;
    }
    response = (const struct ptyterm_error_response *)payload;
    fprintf(stderr, "%s\n", response->message);
    return EXIT_FAILURE;
  }
  default:
    fprintf(stderr, "unexpected response type: %u\n", header.type);
    return EXIT_FAILURE;
  }
}

static int hex_value(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static size_t decode_send_data(const char *input, char *output, size_t output_size) {
  size_t in_offset;
  size_t out_offset;

  in_offset = 0;
  out_offset = 0;
  while (input[in_offset] != '\0') {
    int c;

    if (out_offset >= output_size) {
      fprintf(stderr, "decoded send data too large\n");
      exit(EXIT_FAILURE);
    }

    c = (unsigned char)input[in_offset++];
    if (c == '\\') {
      c = (unsigned char)input[in_offset++];
      switch (c) {
      case '\\': output[out_offset++] = '\\'; break;
      case '^': output[out_offset++] = '^'; break;
      case 'n': output[out_offset++] = '\n'; break;
      case 'r': output[out_offset++] = '\r'; break;
      case 't': output[out_offset++] = '\t'; break;
      case 'e': output[out_offset++] = 0x1b; break;
      case '0': case '1': case '2': case '3':
      case '4': case '5': case '6': case '7': {
        int value;
        int digits;

        value = c - '0';
        digits = 1;
        while (digits < 3 && input[in_offset] >= '0' && input[in_offset] <= '7') {
          value = (value * 8) + (input[in_offset++] - '0');
          digits += 1;
        }
        output[out_offset++] = (char)value;
        break;
      }
      case 'x': {
        int hi;
        int lo;

        hi = hex_value((unsigned char)input[in_offset++]);
        lo = hex_value((unsigned char)input[in_offset++]);
        if (hi < 0 || lo < 0) {
          fprintf(stderr, "invalid hex escape in --send\n");
          exit(EXIT_FAILURE);
        }
        output[out_offset++] = (char)((hi << 4) | lo);
        break;
      }
      case '\0':
        fprintf(stderr, "trailing backslash in --send\n");
        exit(EXIT_FAILURE);
      default:
        fprintf(stderr, "invalid escape in --send: \\%c\n", c);
        exit(EXIT_FAILURE);
      }
      continue;
    }

    if (c == '^') {
      int next;

      next = (unsigned char)input[in_offset++];
      if (next == '\0') {
        fprintf(stderr, "trailing caret in --send\n");
        exit(EXIT_FAILURE);
      }
      if (next == '?') {
        output[out_offset++] = 0x7f;
        continue;
      }
      if (next >= '@' && next <= '_') {
        output[out_offset++] = (char)(next - '@');
        continue;
      }
      if (next >= 'a' && next <= 'z') {
        output[out_offset++] = (char)(next - 'a' + 1);
        continue;
      }
      fprintf(stderr, "invalid caret escape in --send: ^%c\n", next);
      exit(EXIT_FAILURE);
    }

    output[out_offset++] = (char)c;
  }

  return out_offset;
}

static int run_send_client(const char *socket_path, int session_id,
                           const char *send_data) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  char payload[4096];
  char decoded[2048];
  struct ptyterm_send_request *request;
  struct ptyterm_message_header header;
  const struct ptyterm_send_response *response;
  size_t data_size;
  ssize_t payload_size;
  int fd;

  data_size = decode_send_data(send_data, decoded, sizeof(decoded));
  if (sizeof(*request) + data_size > sizeof(payload)) {
    fprintf(stderr, "send request too large\n");
    return EXIT_FAILURE;
  }

  request = (struct ptyterm_send_request *)payload;
  request->session_id = session_id;
  request->data_size = (uint32_t)data_size;
  memcpy(request + 1, decoded, data_size);

  fd = connect_daemon_socket(socket_path, default_socket_path, 1);
  if (fd == -1) {
    perror(socket_path);
    return EXIT_FAILURE;
  }

  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_SEND_REQUEST, payload,
                           (uint32_t)(sizeof(*request) + data_size)) == -1) {
    perror("send");
    close(fd);
    return EXIT_FAILURE;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    perror("recv");
    close(fd);
    return EXIT_FAILURE;
  }

  close(fd);
  if (header.type == PTYTERM_MESSAGE_ERROR) {
    const struct ptyterm_error_response *error_response;

    error_response = (const struct ptyterm_error_response *)payload;
    fprintf(stderr, "%s\n", error_response->message);
    return EXIT_FAILURE;
  }
  if (header.type != PTYTERM_MESSAGE_SEND_RESPONSE ||
      (size_t)payload_size != sizeof(*response)) {
    fprintf(stderr, "invalid send response\n");
    return EXIT_FAILURE;
  }

  response = (const struct ptyterm_send_response *)payload;
  printf("sent %u/%u bytes; %u unsent; resume-offset=%u; blocked=%s; reason=%s\n",
         response->sent_bytes, response->requested_bytes,
         response->unsent_bytes, response->resume_offset,
         response->blocked ? "yes" : "no", response->reason);
  return response->unsent_bytes == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int request_recv_client(const char *socket_path, int session_id,
                               uint32_t recv_size, int recv_peek,
                               char *payload, size_t payload_capacity,
                               const struct ptyterm_recv_response **response_out) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  struct ptyterm_recv_request request;
  struct ptyterm_message_header header;
  const struct ptyterm_recv_response *response;
  ssize_t payload_size;
  int fd;

  request.session_id = session_id;
  request.max_bytes = recv_size;
  request.flags = recv_peek ? PTYTERM_RECV_FLAG_PEEK : 0;
  fd = connect_daemon_socket(socket_path, default_socket_path, 1);
  if (fd == -1) {
    perror(socket_path);
    return EXIT_FAILURE;
  }

  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_RECV_REQUEST, &request,
                           sizeof(request)) == -1) {
    perror("send");
    close(fd);
    return EXIT_FAILURE;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, payload_capacity);
  if (payload_size == -1) {
    perror("recv");
    close(fd);
    return EXIT_FAILURE;
  }

  close(fd);
  if (header.type == PTYTERM_MESSAGE_ERROR) {
    const struct ptyterm_error_response *error_response;

    error_response = (const struct ptyterm_error_response *)payload;
    fprintf(stderr, "%s\n", error_response->message);
    return EXIT_FAILURE;
  }
  if (header.type != PTYTERM_MESSAGE_RECV_RESPONSE ||
      (size_t)payload_size < sizeof(*response)) {
    fprintf(stderr, "invalid recv response\n");
    return EXIT_FAILURE;
  }

  response = (const struct ptyterm_recv_response *)payload;
  if ((size_t)payload_size != sizeof(*response) + response->returned_bytes) {
    fprintf(stderr, "invalid recv response size\n");
    return EXIT_FAILURE;
  }

  *response_out = response;
  return EXIT_SUCCESS;
}

static int print_recv_payload_and_status(
  const struct ptyterm_recv_response *response, const char *reason_override) {
  const char *data;

  data = (const char *)(response + 1);
  if (response->returned_bytes > 0 &&
      fwrite(data, 1, response->returned_bytes, stdout) != response->returned_bytes) {
    perror("fwrite");
    return EXIT_FAILURE;
  }

  print_recv_status_line(response->returned_bytes, response->start_offset,
                         response->end_offset, response->next_recv_offset,
                         response->truncated,
                         reason_override != NULL ? reason_override
                                                 : response->reason);
  return EXIT_SUCCESS;
}

static int run_recv_client(const char *socket_path, int session_id,
                           uint32_t recv_size, int recv_peek,
                           uint64_t recv_timeout_ms,
                           const char *recv_until) {
  char payload[8192];
  const struct ptyterm_recv_response *response;
  uint64_t deadline_ms;
  size_t until_size;

  if (recv_until == NULL && recv_timeout_ms == 0)
    return request_recv_client(socket_path, session_id, recv_size, recv_peek,
                               payload, sizeof(payload), &response) == EXIT_SUCCESS
               ? print_recv_payload_and_status(response, NULL)
               : EXIT_FAILURE;

  deadline_ms = recv_timeout_ms == 0 ? 0 : monotonic_time_ms() + recv_timeout_ms;
  until_size = recv_until == NULL ? 0 : strlen(recv_until);

  for (;;) {
    const char *data;
    const char *match;
    uint32_t consume_size;
    uint64_t now_ms;

    if (request_recv_client(socket_path, session_id, recv_size, 1, payload,
                            sizeof(payload), &response) != EXIT_SUCCESS)
      return EXIT_FAILURE;

    data = (const char *)(response + 1);
    match = recv_until == NULL
                ? (response->returned_bytes > 0 ? data : NULL)
                : find_bytes(data, response->returned_bytes, recv_until,
                             until_size);

    if (match != NULL) {
      consume_size = recv_until == NULL
                         ? response->returned_bytes
                         : (uint32_t)((match - data) + until_size);
      if (request_recv_client(socket_path, session_id, consume_size, 0, payload,
                              sizeof(payload), &response) != EXIT_SUCCESS)
        return EXIT_FAILURE;
      return print_recv_payload_and_status(
          response, recv_until == NULL ? NULL : "match_reached");
    }

    if (recv_until != NULL && response->returned_bytes == recv_size) {
      print_recv_status_line(response->returned_bytes, response->start_offset,
                             response->end_offset, response->next_recv_offset,
                             response->truncated, "size_reached");
      return EXIT_FAILURE;
    }

    if (strcmp(response->reason, "session_exited") == 0) {
      print_recv_status_line(response->returned_bytes, response->start_offset,
                             response->end_offset, response->next_recv_offset,
                             response->truncated, "session_exited");
      return EXIT_FAILURE;
    }

    now_ms = monotonic_time_ms();
    if (recv_timeout_ms == 0 || now_ms >= deadline_ms) {
      print_recv_status_line(response->returned_bytes, response->start_offset,
                             response->end_offset, response->next_recv_offset,
                             response->truncated, "timeout");
      return EXIT_FAILURE;
    }

    usleep(100000);
  }
}

static int run_detach_client(const char *socket_path, int session_id,
                             int status_format) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  char payload[4096];
  struct ptyterm_detach_request request;
  struct ptyterm_message_header header;
  const struct ptyterm_detach_response *response;
  ssize_t payload_size;
  int fd;

  request.session_id = session_id;
  fd = connect_daemon_socket(socket_path, default_socket_path, 1);
  if (fd == -1) {
    perror(socket_path);
    return EXIT_FAILURE;
  }

  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_DETACH_REQUEST, &request,
                           sizeof(request)) == -1) {
    perror("send");
    close(fd);
    return EXIT_FAILURE;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    perror("recv");
    close(fd);
    return EXIT_FAILURE;
  }

  close(fd);
  if (header.type == PTYTERM_MESSAGE_ERROR) {
    const struct ptyterm_error_response *error_response;

    error_response = (const struct ptyterm_error_response *)payload;
    fprintf(stderr, "%s\n", error_response->message);
    return EXIT_FAILURE;
  }
  if (header.type != PTYTERM_MESSAGE_DETACH_RESPONSE ||
      (size_t)payload_size != sizeof(*response)) {
    fprintf(stderr, "invalid detach response\n");
    return EXIT_FAILURE;
  }

  response = (const struct ptyterm_detach_response *)payload;
  print_detach_status(response, status_format);
  return EXIT_SUCCESS;
}

static int request_resize_client(const char *socket_path, int session_id,
                                 uint16_t rows, uint16_t cols,
                                 struct ptyterm_resize_response *response_out) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  char payload[4096];
  struct ptyterm_resize_request request;
  struct ptyterm_message_header header;
  const struct ptyterm_resize_response *response;
  ssize_t payload_size;
  int fd;

  memset(&request, 0, sizeof(request));
  request.session_id = session_id;
  request.rows = rows;
  request.cols = cols;

  fd = connect_daemon_socket(socket_path, default_socket_path, 1);
  if (fd == -1)
    return -1;

  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_RESIZE_REQUEST, &request,
                           sizeof(request)) == -1) {
    close(fd);
    return -1;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    close(fd);
    return -1;
  }

  close(fd);
  if (header.type == PTYTERM_MESSAGE_ERROR) {
    const struct ptyterm_error_response *error_response;

    error_response = (const struct ptyterm_error_response *)payload;
    fprintf(stderr, "%s\n", error_response->message);
    errno = EPROTO;
    return -1;
  }
  if (header.type != PTYTERM_MESSAGE_RESIZE_RESPONSE ||
      (size_t)payload_size != sizeof(*response)) {
    errno = EPROTO;
    return -1;
  }

  response = (const struct ptyterm_resize_response *)payload;
  if (response->session_id != (uint32_t)session_id || response->rows != rows ||
      response->cols != cols) {
    errno = EPROTO;
    return -1;
  }

  if (response_out != NULL)
    *response_out = *response;

  return 0;
}

static int run_resize_client(const char *socket_path, int session_id,
                             uint16_t rows, uint16_t cols, int status_format) {
  struct ptyterm_resize_response response;

  if (request_resize_client(socket_path, session_id, rows, cols, &response) == -1)
    return EXIT_FAILURE;

  print_resize_status(&response, status_format);
  return EXIT_SUCCESS;
}

static int send_current_winsize(const char *socket_path, int session_id, int ifd) {
  struct winsize winsize;

  if (ioctl(ifd, TIOCGWINSZ, &winsize) == -1)
    return -1;
  if (winsize.ws_row == 0 || winsize.ws_col == 0) {
    errno = EINVAL;
    return -1;
  }
  return request_resize_client(socket_path, session_id, winsize.ws_row,
                               winsize.ws_col, NULL);
}

static void attach_size_changed(int sig) {
  (void)sig;
  attach_resize_requested = 1;
}

static int run_daemon_status_client(const char *socket_path,
                                    int status_format) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  char payload[4096];
  struct ptyterm_message_header header;
  const struct ptyterm_daemon_status_response *response;
  ssize_t payload_size;
  int fd;

  fd = connect_daemon_socket(socket_path, default_socket_path, 0);
  if (fd == -1) {
    if (can_autostart_daemon(errno)) {
      print_daemon_status(0, 0, status_format);
      return EXIT_SUCCESS;
    }
    perror(socket_path);
    return EXIT_FAILURE;
  }

  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_DAEMON_STATUS_REQUEST, NULL, 0) == -1) {
    perror("send");
    close(fd);
    return EXIT_FAILURE;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    perror("recv");
    close(fd);
    return EXIT_FAILURE;
  }
  close(fd);

  if (header.type != PTYTERM_MESSAGE_DAEMON_STATUS_RESPONSE ||
      (size_t)payload_size != sizeof(*response)) {
    fprintf(stderr, "invalid daemon-status response\n");
    return EXIT_FAILURE;
  }

  response = (const struct ptyterm_daemon_status_response *)payload;
  print_daemon_status(response->running, response->daemon_pid, status_format);
  return EXIT_SUCCESS;
}

static int run_daemon_stop_client(const char *socket_path, int status_format) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  char payload[4096];
  struct ptyterm_message_header header;
  const struct ptyterm_daemon_shutdown_response *response;
  ssize_t payload_size;
  int fd;

  fd = connect_daemon_socket(socket_path, default_socket_path, 0);
  if (fd == -1) {
    if (can_autostart_daemon(errno)) {
      print_daemon_stop_status(0, 0, status_format);
      return EXIT_SUCCESS;
    }
    perror(socket_path);
    return EXIT_FAILURE;
  }

  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_DAEMON_SHUTDOWN_REQUEST, NULL, 0) == -1) {
    perror("send");
    close(fd);
    return EXIT_FAILURE;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    perror("recv");
    close(fd);
    return EXIT_FAILURE;
  }
  close(fd);

  if (header.type != PTYTERM_MESSAGE_DAEMON_SHUTDOWN_RESPONSE ||
      (size_t)payload_size != sizeof(*response)) {
    fprintf(stderr, "invalid daemon-stop response\n");
    return EXIT_FAILURE;
  }

  response = (const struct ptyterm_daemon_shutdown_response *)payload;
  print_daemon_stop_status(response->stopping, response->daemon_pid,
                           status_format);
  return EXIT_SUCCESS;
}

static int run_attach_client(const char *socket_path, int session_id, int ifd,
                             int ofd) {
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];
  char payload[4096];
  struct ptyterm_attach_request request;
  struct ptyterm_message_header header;
  ssize_t payload_size;
  int fd;
  int itty;
  int stdin_eof;
  struct sigaction sigact;
  char buf_in[4096];
  char buf_out[4096];
  size_t size_in;
  size_t size_out;

  request.session_id = session_id;
  fd = connect_daemon_socket(socket_path, default_socket_path, 1);
  if (fd == -1) {
    perror(socket_path);
    return EXIT_FAILURE;
  }

  if (ptyterm_send_message(fd, PTYTERM_MESSAGE_ATTACH_REQUEST, &request,
                           sizeof(request)) == -1) {
    perror("send");
    close(fd);
    return EXIT_FAILURE;
  }

  payload_size = ptyterm_recv_message(fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    perror("recv");
    close(fd);
    return EXIT_FAILURE;
  }

  if (header.type == PTYTERM_MESSAGE_ERROR) {
    const struct ptyterm_error_response *error_response;

    error_response = (const struct ptyterm_error_response *)payload;
    fprintf(stderr, "%s\n", error_response->message);
    close(fd);
    return EXIT_FAILURE;
  }
  if (header.type != PTYTERM_MESSAGE_ATTACH_RESPONSE) {
    fprintf(stderr, "invalid attach response\n");
    close(fd);
    return EXIT_FAILURE;
  }

  if (set_nonblocking(fd) == -1 || set_nonblocking(ifd) == -1 ||
      set_nonblocking(ofd) == -1) {
    perror("fcntl(O_NONBLOCK)");
    close(fd);
    return EXIT_FAILURE;
  }

  itty = isatty(ifd);
  if (itty) {
    struct termios termios;

    save_termios(ifd);
    termios = saved_termios;
    termios.c_lflag &= ~ICANON & ~ECHO & ~ISIG & ~IEXTEN;
    termios.c_iflag &= ~BRKINT & ~ICRNL & ~INPCK & ~ISTRIP & ~IXON;
    termios.c_cflag &= ~CSIZE & ~PARENB;
    termios.c_cflag |= CS8;
    termios.c_cc[VMIN] = 1;
    termios.c_cc[VTIME] = 0;

    if (ioctl(ifd, TCSETSF, &termios) == -1) {
      perror("ioctl(TCSETSF)");
      close(fd);
      return EXIT_FAILURE;
    }
    g_ifd = ifd;
    atexit(restore_termios_handler);

    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = attach_size_changed;
    sigemptyset(&sigact.sa_mask);
    if (sigaction(SIGWINCH, &sigact, NULL) == -1) {
      perror("sigaction(SIGWINCH)");
      close(fd);
      return EXIT_FAILURE;
    }
    if (send_current_winsize(socket_path, session_id, ifd) == -1) {
      perror("resize");
      close(fd);
      return EXIT_FAILURE;
    }
  }

  attach_resize_requested = 0;
  stdin_eof = 0;
  size_in = 0;
  size_out = 0;
  for (;;) {
    fd_set rfds;
    fd_set wfds;
    int maxfd;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    maxfd = fd;

    if (!stdin_eof && size_in < sizeof(buf_in)) {
      FD_SET(ifd, &rfds);
      if (maxfd < ifd)
        maxfd = ifd;
    }
    if (size_in > 0)
      FD_SET(fd, &wfds);
    FD_SET(fd, &rfds);
    if (size_out > 0) {
      FD_SET(ofd, &wfds);
      if (maxfd < ofd)
        maxfd = ofd;
    }

    if (select(maxfd + 1, &rfds, &wfds, NULL, NULL) == -1) {
      if (errno == EINTR)
        goto maybe_resize;
      perror("select");
      close(fd);
      return EXIT_FAILURE;
    }

    if (FD_ISSET(fd, &wfds)) {
      ssize_t written;

      written = write(fd, buf_in, size_in);
      if (written == -1) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        perror("write");
        close(fd);
        return EXIT_FAILURE;
      }
      if ((size_t)written < size_in)
        memmove(buf_in, buf_in + written, size_in - (size_t)written);
      size_in -= (size_t)written;
    }

    if (size_out > 0 && FD_ISSET(ofd, &wfds)) {
      ssize_t written;

      written = write(ofd, buf_out, size_out);
      if (written == -1) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        perror("write");
        close(fd);
        return EXIT_FAILURE;
      }
      if ((size_t)written < size_out)
        memmove(buf_out, buf_out + written, size_out - (size_t)written);
      size_out -= (size_t)written;
    }

    if (FD_ISSET(fd, &rfds) && size_out < sizeof(buf_out)) {
      ssize_t size;

      size = read(fd, buf_out + size_out, sizeof(buf_out) - size_out);
      if (size == -1) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        perror("read");
        close(fd);
        return EXIT_FAILURE;
      }
      if (size == 0) {
        close(fd);
        return EXIT_SUCCESS;
      }
      size_out += (size_t)size;
    }

    if (!stdin_eof && FD_ISSET(ifd, &rfds) && size_in < sizeof(buf_in)) {
      ssize_t size;

      size = read(ifd, buf_in + size_in, sizeof(buf_in) - size_in);
      if (size == -1) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        perror("read");
        close(fd);
        return EXIT_FAILURE;
      }
      if (size == 0) {
        stdin_eof = 1;
      } else {
        size_in += (size_t)size;
      }
    }

maybe_resize:
    if (itty && attach_resize_requested) {
      attach_resize_requested = 0;
      if (send_current_winsize(socket_path, session_id, ifd) == -1) {
        perror("resize");
        close(fd);
        return EXIT_FAILURE;
      }
    }
  }
}

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
static void restore_termios_handler() { restore_termios(g_ifd); }

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
  if (set_nonblocking(fd) == -1) {
    perror("fcntl(O_NONBLOCK)");
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

  int ifd = STDIN_FILENO;
  int ofd = STDOUT_FILENO;
  int efd = STDERR_FILENO;

  const char *ifile = NULL;
  const char *ofile = NULL;
  const char *afile = NULL;
  const char *socket_path = NULL;
  int buffer_info_requested = 0;
  int attach_requested = 0;
  int create_requested = 0;
  int daemon_status_requested = 0;
  int daemon_stop_requested = 0;
  int detach_requested = 0;
  int help_requested = 0;
  int help_format = PTYTERM_HELP_FORMAT_TEXT;
  int list_requested = 0;
  int recv_peek = 0;
  const char *recv_until = NULL;
  uint64_t recv_timeout_ms = 0;
  int resize_requested = 0;
  int recv_requested = 0;
  int status_format = PTYTERM_STATUS_FORMAT_KV;
  const char *send_data = NULL;
  uint32_t recv_size = 4096;
  int session_id = PTYTERM_SESSION_ALL;

  g_program_path = argv[0];

  setlocale(LC_ALL, "");
  while (1) {
    /// @brief オプション
    int opt;
    /// @brief オプションのインデックス
    int optindex;
    enum {
      OPT_SEND = 1000,
      OPT_RECV,
      OPT_RECV_SIZE,
      OPT_RECV_TIMEOUT,
      OPT_RECV_UNTIL,
      OPT_PEEK,
      OPT_DAEMON_STATUS,
      OPT_DAEMON_STOP,
      OPT_HELP_FORMAT,
      OPT_STATUS_FORMAT,
    };

    char *p;
    /// @brief オプションの定義
    static struct option longopts[] = {{"version", no_argument, NULL, 'V'},
                       {"help", no_argument, NULL, 'h'},
               {"attach", no_argument, NULL, 'A'},
                       {"create", no_argument, NULL, 'C'},
                     {"daemon-status", no_argument, NULL, OPT_DAEMON_STATUS},
                     {"daemon-stop", no_argument, NULL, OPT_DAEMON_STOP},
                     {"help-format", required_argument, NULL, OPT_HELP_FORMAT},
                       {"status-format", required_argument, NULL, OPT_STATUS_FORMAT},
               {"detach", no_argument, NULL, 'D'},
                       {"resize", no_argument, NULL, 'R'},
                       {"send", required_argument, NULL, OPT_SEND},
                       {"recv", no_argument, NULL, OPT_RECV},
                       {"recv-size", required_argument, NULL, OPT_RECV_SIZE},
                       {"recv-timeout", required_argument, NULL, OPT_RECV_TIMEOUT},
                       {"recv-until", required_argument, NULL, OPT_RECV_UNTIL},
                       {"peek", no_argument, NULL, OPT_PEEK},
                       {"socket", required_argument, NULL, 's'},
                       {"buffer-info", no_argument, NULL, 'B'},
                       {"list", no_argument, NULL, 'L'},
                       {"session", required_argument, NULL, 'S'},
                       {"stdin", required_argument, NULL, 'i'},
                       {"input", required_argument, NULL, 'i'},
                       {"stdout", required_argument, NULL, 'o'},
                       {"output", required_argument, NULL, 'o'},
                       {"stdout-append", required_argument, NULL,
                      'a'},
                       {"append", required_argument, NULL, 'a'},
                       {"cols", required_argument, NULL, 'c'},
                       {"lines", required_argument, NULL, 'l'},
                       {"rows", required_argument, NULL, 'l'},
                       {NULL, 0, NULL, 0}};

    /// @brief オプションを取得する
    opt = getopt_long(argc, argv, "+:VhACDRs:BLi:o:a:c:l:S:", longopts, &optindex);
    if (opt == -1)
      break;

    switch (opt) {
    case 'V':
      printf("%s\n", PACKAGE_STRING);
      return EXIT_SUCCESS;
    case 'h':
      help_requested = 1;
      break;
    case 'A':
      attach_requested = 1;
      break;
    case 'C':
      create_requested = 1;
      break;
    case OPT_DAEMON_STATUS:
      daemon_status_requested = 1;
      break;
    case OPT_DAEMON_STOP:
      daemon_stop_requested = 1;
      break;
    case OPT_HELP_FORMAT:
      help_format = parse_help_format(optarg);
      if (help_format < 0)
        return usage_error(argv[0], "unsupported help format: %s", optarg);
      help_requested = 1;
      break;
    case OPT_STATUS_FORMAT:
      status_format = parse_status_format(optarg);
      if (status_format < 0)
        return usage_error(argv[0], "unsupported status format: %s", optarg);
      break;
    case 'D':
      detach_requested = 1;
      break;
    case 'R':
      resize_requested = 1;
      break;
    case OPT_SEND:
      send_data = optarg;
      break;
    case OPT_RECV:
      recv_requested = 1;
      break;
    case OPT_PEEK:
      recv_peek = 1;
      break;
    case OPT_RECV_SIZE:
      recv_size = (uint32_t)strtoul(optarg, &p, 0);
      if (optarg == p || *p != '\0' || recv_size == 0)
        return usage_error(argv[0], "invalid recv-size: %s", optarg);
      break;
    case OPT_RECV_TIMEOUT:
      if (parse_duration_ms(optarg, &recv_timeout_ms) == -1)
        return usage_error(argv[0], "invalid recv-timeout: %s", optarg);
      break;
    case OPT_RECV_UNTIL:
      if (*optarg == '\0')
        return usage_error(argv[0], "recv-until must not be empty");
      recv_until = optarg;
      break;
    case 's':
      socket_path = optarg;
      break;
    case 'L':
      list_requested = 1;
      break;
    case 'B':
      buffer_info_requested = 1;
      break;
    case 'i':
      ifile = optarg;
      break;
    case 'o':
      ofile = optarg;
      break;
    case 'a':
      afile = optarg;
      break;
    case 'c':
      opt_cols = strtol(optarg, &p, 0);
      if (optarg == p || *p != '\0' || opt_cols <= 0)
        return usage_error(argv[0], "invalid cols: %s", optarg);
      break;
    case 'l':
      opt_lines = strtol(optarg, &p, 0);
      if (optarg == p || *p != '\0' || opt_lines <= 0)
        return usage_error(argv[0], "invalid lines: %s", optarg);
      break;
    case 'S':
      session_id = strtol(optarg, &p, 0);
      if (optarg == p || *p != '\0' || session_id <= 0)
        return usage_error(argv[0], "invalid session: %s", optarg);
      break;
    case ':':
      return usage_error(argv[0], "option requires an argument: %s",
                         argv[optind - 1]);
    case '?':
      return usage_error(argv[0], "unknown option or invalid use: %s",
                         argv[optind - 1]);
    default:
      return usage_error(argv[0], "invalid command-line usage");
    }
  }

  if (help_requested) {
    print_help(stdout, argv[0], help_format);
    return EXIT_SUCCESS;
  }

  if (ofile && afile) {
    return usage_error(argv[0], "output and append options are exclusive");
  }

  if (recv_peek && !recv_requested)
    return usage_error(argv[0], "--peek requires --recv");
  if ((recv_timeout_ms != 0 || recv_until != NULL) && !recv_requested)
    return usage_error(argv[0], "recv wait options require --recv");

  if ((attach_requested != 0) + (create_requested != 0) +
      (daemon_status_requested != 0) + (daemon_stop_requested != 0) +
      (detach_requested != 0) + (list_requested != 0) +
      (resize_requested != 0) +
          (buffer_info_requested != 0) + (recv_requested != 0) +
          (send_data != NULL) >
      1) {
    return usage_error(argv[0], "select only one management operation");
  }

  if (attach_requested || create_requested || daemon_status_requested ||
      daemon_stop_requested || detach_requested ||
      resize_requested ||
      list_requested || buffer_info_requested ||
      recv_requested || send_data != NULL) {
    if ((ifile || ofile || afile ||
         ((opt_cols > 0 || opt_lines > 0) && !resize_requested)) &&
        !attach_requested) {
      return usage_error(
          argv[0],
          "run options are not supported with management operations");
    }
    if (!create_requested && optind != argc) {
      return usage_error(
          argv[0],
          "management operations do not accept environment or command arguments");
    }
    if (create_requested) {
      if (session_id != PTYTERM_SESSION_ALL) {
        return usage_error(argv[0], "--create does not accept --session");
      }
      return run_create_client(socket_path, argc - optind, argv + optind,
                               status_format);
    }
    if (daemon_status_requested)
      return run_daemon_status_client(socket_path, status_format);
    if (daemon_stop_requested)
      return run_daemon_stop_client(socket_path, status_format);
    if (list_requested)
      return run_list_client(socket_path, session_id);
    if (session_id == PTYTERM_SESSION_ALL) {
      return usage_error(argv[0],
                         "this management operation requires --session=ID");
    }
    if (attach_requested)
      return run_attach_client(socket_path, session_id, ifd, ofd);
    if (detach_requested)
      return run_detach_client(socket_path, session_id, status_format);
    if (resize_requested) {
      if (opt_cols <= 0 || opt_lines <= 0) {
        return usage_error(argv[0],
                           "--resize requires --rows=N and --cols=N");
      }
      return run_resize_client(socket_path, session_id, (uint16_t)opt_lines,
                               (uint16_t)opt_cols, status_format);
    }
    if (send_data != NULL)
      return run_send_client(socket_path, session_id, send_data);
    if (recv_requested)
      return run_recv_client(socket_path, session_id, recv_size, recv_peek,
                             recv_timeout_ms, recv_until);
    return run_buffer_info_client(socket_path, session_id, status_format);
  }

  /// @brief 環境変数を設定する
  argoffset = optind;
  /// @brief 環境変数を設定する
  while (argoffset < argc && index(argv[argoffset], '='))
    putenv(argv[argoffset++]);

  if (ifile && strcmp(ifile, "-") != 0) {
    ifd = open(ifile, O_RDONLY);
    if (ifd == -1) {
      perror(ifile);
      exit(EXIT_FAILURE);
    }
  }

  if (ofile && strcmp(ofile, "-") != 0) {
    ofd = open(ofile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ofd == -1) {
      perror(ofile);
      exit(EXIT_FAILURE);
    }
  }

  if (afile) {
    ofd = open(afile, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (ofd == -1) {
      perror(afile);
      exit(EXIT_FAILURE); 
    }
  }

  /// @brief 標準入力が端末かどうか
  itty = isatty(ifd);
  if (itty) {
    save_termios(ifd);
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
      copy_winsize(ifd, sfd);
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
    dup2(sfd, ifd);
    // 標準出力をスレーブ端末とする
    dup2(sfd, ofd);
    // 標準エラー出力をスレーブ端末とする
    dup2(sfd, efd);
    // スレーブ端末は閉じる
    if (sfd != ifd && sfd != ofd && sfd != efd) {
      close(sfd);
    }
    // 引数をコマンドとして実行する
    if (argc == argoffset) {
      char *const shell = getenv("SHELL") ?: "/bin/sh";
      char *const argv_shell[] = {shell, NULL};

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

      if (ioctl(ifd, TCSETSF, &termios) == -1) {
        perror("ioctl(TCSETSF)");
        exit(EXIT_FAILURE);
      }

      // プログラム終了時に元に戻す
      g_ifd = ifd;
      atexit(restore_termios_handler);
      srcfd = ifd;
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

    if (set_nonblocking(ifd) == -1 || set_nonblocking(ofd) == -1) {
      perror("fcntl(O_NONBLOCK)");
      exit(EXIT_FAILURE);
    }

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
        FD_SET(ifd, &rfds);
        if (maxfd < ifd)
          maxfd = ifd;
      }
      // 擬似端末 -> 標準出力
      // 用のバッファにデータがあれば書込可能かチェック対象とする
      if (siz1 > 0) {
        FD_SET(ofd, &wfds);
        if (maxfd < ofd)
          maxfd = ofd;
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
          if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
          perror("write");
          exit(EXIT_FAILURE);
        }
        if (siz < siz2) {
          memmove(buf2, buf2 + siz, siz2 - siz);
        }
        siz2 -= siz;
      }

      // 標準出力に書込
      if (FD_ISSET(ofd, &wfds)) {
        ssize_t siz = write(ofd, buf1, siz1);
        if (siz == -1) {
          if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
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
          if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
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
      if (FD_ISSET(ifd, &rfds)) {
        ssize_t siz = read(ifd, buf2 + siz2, sizeof(buf2) - siz2);
        if (siz == -1) {
          if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
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
