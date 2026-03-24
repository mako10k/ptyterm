#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define PACKAGE_STRING "ptytermd"
#endif

#include "ptyterm-control.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ptyterm_session {
  uint32_t id;
  uint32_t state;
  int32_t child_pid;
  int32_t exit_status;
  int master_fd;
  uint32_t buffer_capacity;
  uint32_t buffer_used;
  uint32_t dropped_bytes;
  uint32_t paused_on_full;
  size_t ring_start;
  size_t ring_len;
  char *output_ring;
  char command[PTYTERM_COMMAND_MAX];
};

struct ptyterm_daemon_state {
  int server_fd;
  char socket_path[PTYTERM_SOCKET_PATH_MAX];
  uint32_t output_buffer;
  struct ptyterm_session sessions[32];
  size_t session_count;
};

static volatile sig_atomic_t stop_requested = 0;
static char cleanup_socket_path[PTYTERM_SOCKET_PATH_MAX];

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

static void request_stop(int sig) {
  (void)sig;
  stop_requested = 1;
}

static void cleanup_socket(void) {
  if (cleanup_socket_path[0] != '\0')
    unlink(cleanup_socket_path);
}

static void append_output(struct ptyterm_session *session, const char *buffer,
                          size_t size) {
  size_t i;

  if (session->output_ring == NULL || session->buffer_capacity == 0)
    return;

  for (i = 0; i < size; ++i) {
    if (session->ring_len == session->buffer_capacity) {
      session->ring_start = (session->ring_start + 1) % session->buffer_capacity;
      session->ring_len -= 1;
      session->dropped_bytes += 1;
    }
    session->output_ring[(session->ring_start + session->ring_len) %
                         session->buffer_capacity] = buffer[i];
    session->ring_len += 1;
  }
  session->buffer_used = (uint32_t)session->ring_len;
}

static int next_session_id(const struct ptyterm_daemon_state *state) {
  uint32_t candidate;

  for (candidate = 1; candidate < UINT32_MAX; ++candidate) {
    size_t i;
    int used;

    used = 0;
    for (i = 0; i < state->session_count; ++i) {
      if (state->sessions[i].id == candidate) {
        used = 1;
        break;
      }
    }
    if (!used)
      return (int)candidate;
  }
  errno = ENOSPC;
  return -1;
}

static int open_session_pty(char **slave_name_out) {
  int master_fd;
  char *slave_name;

  master_fd = open("/dev/ptmx", O_RDWR);
  if (master_fd == -1)
    return -1;
  if (grantpt(master_fd) == -1) {
    close(master_fd);
    return -1;
  }
  if (unlockpt(master_fd) == -1) {
    close(master_fd);
    return -1;
  }
  slave_name = ptsname(master_fd);
  if (slave_name == NULL) {
    close(master_fd);
    return -1;
  }
  *slave_name_out = slave_name;
  return master_fd;
}

static int open_slave_fd(const char *slave_name) {
  return open(slave_name, O_RDWR);
}

static void join_command(char *buffer, size_t buffer_size, uint32_t argc,
                         char *const argv[]) {
  size_t offset;
  uint32_t i;

  if (argc == 0) {
    const char *shell;

    shell = getenv("SHELL");
    if (shell == NULL || *shell == '\0')
      shell = "/bin/sh";
    snprintf(buffer, buffer_size, "%s", shell);
    return;
  }

  offset = 0;
  buffer[0] = '\0';
  for (i = 0; i < argc; ++i) {
    int written;

    written = snprintf(buffer + offset, buffer_size - offset, "%s%s",
                       i == 0 ? "" : " ", argv[i]);
    if (written < 0)
      break;
    if ((size_t)written >= buffer_size - offset) {
      offset = buffer_size - 1;
      break;
    }
    offset += (size_t)written;
  }
  buffer[offset] = '\0';
}

static int spawn_session(struct ptyterm_daemon_state *state, uint32_t argc,
                         char *const argv[], struct ptyterm_create_response *response) {
  struct ptyterm_session *session;
  char *slave_name;
  int master_fd;
  pid_t child_pid;
  int session_id;

  if (state->session_count >= sizeof(state->sessions) / sizeof(state->sessions[0])) {
    errno = ENOSPC;
    return -1;
  }

  session_id = next_session_id(state);
  if (session_id == -1)
    return -1;

  master_fd = open_session_pty(&slave_name);
  if (master_fd == -1)
    return -1;

  child_pid = fork();
  if (child_pid == -1) {
    close(master_fd);
    return -1;
  }

  if (child_pid == 0) {
    int slave_fd;

    close(master_fd);
    if (setsid() == -1) {
      perror("setsid");
      _exit(127);
    }

    slave_fd = open_slave_fd(slave_name);
    if (slave_fd == -1) {
      perror("open slave");
      _exit(127);
    }

    if (dup2(slave_fd, STDIN_FILENO) == -1 || dup2(slave_fd, STDOUT_FILENO) == -1 ||
        dup2(slave_fd, STDERR_FILENO) == -1) {
      perror("dup2");
      _exit(127);
    }
    if (slave_fd > STDERR_FILENO)
      close(slave_fd);

    if (argc == 0) {
      char *const shell_argv[] = {(char *)(getenv("SHELL") ?: "/bin/sh"), NULL};

      execvp(shell_argv[0], shell_argv);
      perror(shell_argv[0]);
      _exit(127);
    }

    execvp(argv[0], argv);
    perror(argv[0]);
    _exit(127);
  }

  session = &state->sessions[state->session_count++];
  memset(session, 0, sizeof(*session));
  session->id = (uint32_t)session_id;
  session->state = PTYTERM_SESSION_DETACHED;
  session->child_pid = child_pid;
  session->exit_status = -1;
  session->master_fd = master_fd;
  session->buffer_capacity = state->output_buffer;
  session->output_ring = calloc(1, session->buffer_capacity);
  if (session->output_ring == NULL) {
    close(master_fd);
    kill(child_pid, SIGTERM);
    waitpid(child_pid, NULL, 0);
    state->session_count -= 1;
    return -1;
  }
  join_command(session->command, sizeof(session->command), argc, argv);

  response->session_id = session->id;
  response->state = session->state;
  response->child_pid = session->child_pid;
  return 0;
}

static void reap_children(struct ptyterm_daemon_state *state) {
  int status;
  pid_t pid;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    size_t i;

    for (i = 0; i < state->session_count; ++i) {
      if (state->sessions[i].child_pid != pid)
        continue;
      state->sessions[i].state = PTYTERM_SESSION_EXITED;
      if (WIFEXITED(status)) {
        state->sessions[i].exit_status = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        state->sessions[i].exit_status = 128 + WTERMSIG(status);
      } else {
        state->sessions[i].exit_status = status;
      }
      break;
    }
  }
}

static void drain_session_output(struct ptyterm_session *session) {
  char buffer[1024];
  ssize_t size;

  if (session->master_fd < 0)
    return;

  size = read(session->master_fd, buffer, sizeof(buffer));
  if (size > 0) {
    append_output(session, buffer, (size_t)size);
    return;
  }

  if (size == 0 || errno == EIO) {
    close(session->master_fd);
    session->master_fd = -1;
    return;
  }
}

static void cleanup_state(struct ptyterm_daemon_state *state) {
  size_t i;

  for (i = 0; i < state->session_count; ++i) {
    if (state->sessions[i].master_fd >= 0)
      close(state->sessions[i].master_fd);
    if (state->sessions[i].state != PTYTERM_SESSION_EXITED &&
        state->sessions[i].child_pid > 0) {
      kill(state->sessions[i].child_pid, SIGTERM);
      waitpid(state->sessions[i].child_pid, NULL, 0);
    }
    free(state->sessions[i].output_ring);
    state->sessions[i].output_ring = NULL;
  }
}

static void install_signal_handlers(void) {
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = request_stop;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGINT, &sa, NULL) == -1) {
    perror("sigaction(SIGINT)");
    exit(EXIT_FAILURE);
  }
  if (sigaction(SIGTERM, &sa, NULL) == -1) {
    perror("sigaction(SIGTERM)");
    exit(EXIT_FAILURE);
  }
}

static int send_error_response(int client_fd, int error_code,
                               const char *message) {
  struct ptyterm_error_response response;

  memset(&response, 0, sizeof(response));
  response.error_code = error_code;
  snprintf(response.message, sizeof(response.message), "%s", message);
  return ptyterm_send_message(client_fd, PTYTERM_MESSAGE_ERROR, &response,
                              sizeof(response));
}

static int send_list_response(int client_fd,
                              const struct ptyterm_daemon_state *state,
                              int requested_session_id) {
  struct ptyterm_list_response *response;
  struct ptyterm_session_summary *summary;
  size_t i;
  size_t match_count;
  size_t payload_size;

  match_count = 0;
  for (i = 0; i < state->session_count; ++i) {
    if (requested_session_id == PTYTERM_SESSION_ALL ||
        state->sessions[i].id == (uint32_t)requested_session_id) {
      ++match_count;
    }
  }

  payload_size = sizeof(*response) +
                 match_count * sizeof(struct ptyterm_session_summary);
  response = calloc(1, payload_size);
  if (response == NULL)
    return -1;

  response->session_count = (uint32_t)match_count;
  summary = (struct ptyterm_session_summary *)(response + 1);
  for (i = 0; i < state->session_count; ++i) {
    if (requested_session_id != PTYTERM_SESSION_ALL &&
        state->sessions[i].id != (uint32_t)requested_session_id) {
      continue;
    }
    summary->id = state->sessions[i].id;
    summary->state = state->sessions[i].state;
    summary->child_pid = state->sessions[i].child_pid;
    summary->exit_status = state->sessions[i].exit_status;
    snprintf(summary->command, sizeof(summary->command), "%s",
             state->sessions[i].command);
    ++summary;
  }

  i = ptyterm_send_message(client_fd, PTYTERM_MESSAGE_LIST_RESPONSE, response,
                           (uint32_t)payload_size);
  free(response);
  return (int)i;
}

static const struct ptyterm_session *find_session(
    const struct ptyterm_daemon_state *state, int requested_session_id) {
  size_t i;

  for (i = 0; i < state->session_count; ++i) {
    if (state->sessions[i].id == (uint32_t)requested_session_id)
      return &state->sessions[i];
  }
  return NULL;
}

static int send_buffer_info_response(
    int client_fd, const struct ptyterm_daemon_state *state,
    int requested_session_id) {
  const struct ptyterm_session *session;
  struct ptyterm_buffer_info_response response;

  session = find_session(state, requested_session_id);
  if (session == NULL) {
    errno = ENOENT;
    return -1;
  }

  memset(&response, 0, sizeof(response));
  response.id = session->id;
  response.state = session->state;
  response.buffer_capacity = session->buffer_capacity;
  response.buffer_used = session->buffer_used;
  response.dropped_bytes = session->dropped_bytes;
  response.paused_on_full = session->paused_on_full;
  return ptyterm_send_message(client_fd, PTYTERM_MESSAGE_BUFFER_INFO_RESPONSE,
                              &response, sizeof(response));
}

static int handle_create_request(int client_fd, struct ptyterm_daemon_state *state,
                                 const void *payload, size_t payload_size) {
  const struct ptyterm_create_request *request;
  char **argv;
  char *strings;
  size_t strings_size;
  size_t offset;
  uint32_t i;
  struct ptyterm_create_response response;

  if (payload_size < sizeof(*request)) {
    errno = EPROTO;
    return -1;
  }

  request = (const struct ptyterm_create_request *)payload;
  strings = (char *)(request + 1);
  strings_size = payload_size - sizeof(*request);
  argv = calloc(request->argc + 1, sizeof(*argv));
  if (argv == NULL)
    return -1;

  offset = 0;
  for (i = 0; i < request->argc; ++i) {
    size_t len;

    if (offset >= strings_size) {
      free(argv);
      errno = EPROTO;
      return -1;
    }
    len = strnlen(strings + offset, strings_size - offset);
    if (offset + len >= strings_size) {
      free(argv);
      errno = EPROTO;
      return -1;
    }
    argv[i] = strings + offset;
    offset += len + 1;
  }
  argv[request->argc] = NULL;

  if (spawn_session(state, request->argc, argv, &response) == -1) {
    free(argv);
    return -1;
  }
  free(argv);

  return ptyterm_send_message(client_fd, PTYTERM_MESSAGE_CREATE_RESPONSE,
                              &response, sizeof(response));
}

static void handle_client(int client_fd, struct ptyterm_daemon_state *state) {
  char payload[4096];
  struct ptyterm_message_header header;
  ssize_t payload_size;

  payload_size = ptyterm_recv_message(client_fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    send_error_response(client_fd, errno, strerror(errno));
    return;
  }

  switch (header.type) {
  case PTYTERM_MESSAGE_LIST_REQUEST: {
    struct ptyterm_list_request request;

    if ((size_t)payload_size != sizeof(request)) {
      send_error_response(client_fd, EPROTO, "invalid list request size");
      return;
    }
    memcpy(&request, payload, sizeof(request));
    if (send_list_response(client_fd, state, request.session_id) == -1) {
      send_error_response(client_fd, errno, strerror(errno));
    }
    return;
  }
  case PTYTERM_MESSAGE_BUFFER_INFO_REQUEST: {
    struct ptyterm_buffer_info_request request;

    if ((size_t)payload_size != sizeof(request)) {
      send_error_response(client_fd, EPROTO,
                          "invalid buffer-info request size");
      return;
    }
    memcpy(&request, payload, sizeof(request));
    if (request.session_id <= 0) {
      send_error_response(client_fd, EINVAL, "invalid session id");
      return;
    }
    if (send_buffer_info_response(client_fd, state, request.session_id) == -1) {
      if (errno == ENOENT) {
        send_error_response(client_fd, errno, "session not found");
      } else {
        send_error_response(client_fd, errno, strerror(errno));
      }
    }
    return;
  }
  case PTYTERM_MESSAGE_CREATE_REQUEST:
    if (handle_create_request(client_fd, state, payload, (size_t)payload_size) == -1) {
      send_error_response(client_fd, errno, strerror(errno));
    }
    return;
  default:
    send_error_response(client_fd, ENOTSUP, "unsupported request type");
    return;
  }
}

int main(int argc, char *const argv[]) {
  struct ptyterm_daemon_state state;
  const char *socket_path = NULL;
  const char *overflow = "drop";
  unsigned long output_buffer = 4096UL;
  char default_socket_path[PTYTERM_SOCKET_PATH_MAX];

  memset(&state, 0, sizeof(state));
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
      printf("  -s, --socket=PATH          control socket path\n");
      printf("  -b, --output-buffer=SIZE   per-session output buffer size "
             "(default: 4096)\n");
      printf("  -o, --overflow=drop|pause  output buffer overflow policy "
             "(default: drop)\n");
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

  (void)overflow;

  if (output_buffer > UINT32_MAX) {
    fprintf(stderr, "output-buffer too large: %lu\n", output_buffer);
    exit(EXIT_FAILURE);
  }
  state.output_buffer = (uint32_t)output_buffer;

  if (socket_path == NULL) {
    if (ptyterm_default_socket_path(default_socket_path,
                                    sizeof(default_socket_path)) == -1) {
      perror("default socket path");
      exit(EXIT_FAILURE);
    }
    socket_path = default_socket_path;
  }

  snprintf(state.socket_path, sizeof(state.socket_path), "%s", socket_path);
  snprintf(cleanup_socket_path, sizeof(cleanup_socket_path), "%s", socket_path);
  atexit(cleanup_socket);
  install_signal_handlers();

  state.server_fd = ptyterm_bind_listen_socket(socket_path);
  if (state.server_fd == -1) {
    perror(socket_path);
    exit(EXIT_FAILURE);
  }

  while (!stop_requested) {
    fd_set rfds;
    int maxfd;
    size_t i;
    int client_fd;

    FD_ZERO(&rfds);
    FD_SET(state.server_fd, &rfds);
    maxfd = state.server_fd;
    for (i = 0; i < state.session_count; ++i) {
      if (state.sessions[i].master_fd < 0)
        continue;
      FD_SET(state.sessions[i].master_fd, &rfds);
      if (maxfd < state.sessions[i].master_fd)
        maxfd = state.sessions[i].master_fd;
    }

    if (select(maxfd + 1, &rfds, NULL, NULL, NULL) == -1) {
      if (errno == EINTR)
        continue;
      perror("select");
      exit(EXIT_FAILURE);
    }

    for (i = 0; i < state.session_count; ++i) {
      if (state.sessions[i].master_fd >= 0 &&
          FD_ISSET(state.sessions[i].master_fd, &rfds)) {
        drain_session_output(&state.sessions[i]);
      }
    }
    reap_children(&state);

    if (!FD_ISSET(state.server_fd, &rfds))
      continue;

    client_fd = accept(state.server_fd, NULL, NULL);
    if (client_fd == -1) {
      if (errno == EINTR)
        continue;
      perror("accept");
      exit(EXIT_FAILURE);
    }

    handle_client(client_fd, &state);
    close(client_fd);
  }

  close(state.server_fd);
  state.server_fd = -1;
  cleanup_state(&state);
  cleanup_socket();
  cleanup_socket_path[0] = '\0';
  return EXIT_SUCCESS;
}