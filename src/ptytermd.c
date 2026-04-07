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
  int client_fd;
  size_t pending_input_size;
  size_t pending_input_capacity;
  char *pending_input;
  size_t pending_output_size;
  size_t pending_output_capacity;
  char *pending_output;
  uint64_t recv_offset;
  uint64_t send_stream_offset;
  uint64_t total_output_bytes;
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

static int set_nonblocking(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL);
  if (flags == -1)
    return -1;
  if ((flags & O_NONBLOCK) != 0)
    return 0;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

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
    session->total_output_bytes += 1;
  }
  session->buffer_used = (uint32_t)session->ring_len;
}

static void close_attached_client(struct ptyterm_session *session) {
  if (session->client_fd >= 0) {
    close(session->client_fd);
    session->client_fd = -1;
  }
  session->pending_input_size = 0;
  session->pending_output_size = 0;
  if (session->state == PTYTERM_SESSION_ATTACHED)
    session->state = PTYTERM_SESSION_DETACHED;
}

static int append_pending_data(char **buffer, size_t *size, size_t *capacity,
                               const char *data, size_t data_size) {
  char *new_buffer;
  size_t new_capacity;

  if (data_size == 0)
    return 0;
  if (*capacity - *size >= data_size) {
    memcpy(*buffer + *size, data, data_size);
    *size += data_size;
    return 0;
  }

  new_capacity = *capacity == 0 ? 4096 : *capacity;
  while (new_capacity - *size < data_size) {
    if (new_capacity > SIZE_MAX / 2) {
      errno = ENOMEM;
      return -1;
    }
    new_capacity *= 2;
  }

  new_buffer = realloc(*buffer, new_capacity);
  if (new_buffer == NULL)
    return -1;

  memcpy(new_buffer + *size, data, data_size);
  *buffer = new_buffer;
  *capacity = new_capacity;
  *size += data_size;
  return 0;
}

static int flush_pending_data(int fd, char *buffer, size_t *size) {
  ssize_t written;

  if (*size == 0)
    return 0;

  written = write(fd, buffer, *size);
  if (written == -1) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    return -1;
  }
  if ((size_t)written < *size)
    memmove(buffer, buffer + written, *size - (size_t)written);
  *size -= (size_t)written;
  return 0;
}

static int append_pending_output(struct ptyterm_session *session,
                                 const char *data, size_t data_size) {
  size_t limit;

  limit = session->buffer_capacity;
  if (limit < 4096)
    limit = 4096;
  if (session->pending_output_size > limit ||
      data_size > limit - session->pending_output_size) {
    errno = ENOBUFS;
    return -1;
  }
  return append_pending_data(&session->pending_output, &session->pending_output_size,
                             &session->pending_output_capacity, data, data_size);
}

static int apply_session_winsize(struct ptyterm_session *session,
                                 uint16_t rows, uint16_t cols) {
  struct winsize winsize;

  if (session->master_fd < 0) {
    errno = EPIPE;
    return -1;
  }

  memset(&winsize, 0, sizeof(winsize));
  winsize.ws_row = rows;
  winsize.ws_col = cols;
  if (ioctl(session->master_fd, TIOCSWINSZ, &winsize) == -1)
    return -1;
  return 0;
}

static uint64_t oldest_available_offset(const struct ptyterm_session *session) {
  return session->total_output_bytes - session->ring_len;
}

static size_t copy_output_from_offset(const struct ptyterm_session *session,
                                      uint64_t offset, char *buffer,
                                      size_t max_bytes) {
  size_t available;
  size_t copied;
  size_t ring_offset;

  if (offset < oldest_available_offset(session) || offset > session->total_output_bytes)
    return 0;

  available = (size_t)(session->total_output_bytes - offset);
  if (available > max_bytes)
    available = max_bytes;

  ring_offset = (size_t)(offset - oldest_available_offset(session));
  copied = 0;
  while (copied < available) {
    size_t chunk;
    size_t start;

    start = (session->ring_start + ring_offset + copied) % session->buffer_capacity;
    chunk = available - copied;
    if (start + chunk > session->buffer_capacity)
      chunk = session->buffer_capacity - start;
    memcpy(buffer + copied, session->output_ring + start, chunk);
    copied += chunk;
  }

  return copied;
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
  if (set_nonblocking(master_fd) == -1) {
    close(master_fd);
    return -1;
  }
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
  session->client_fd = -1;
  session->pending_input_size = 0;
  session->pending_input_capacity = 0;
  session->pending_input = NULL;
  session->pending_output_size = 0;
  session->pending_output_capacity = 0;
  session->pending_output = NULL;
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
      close_attached_client(&state->sessions[i]);
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
    if (session->client_fd >= 0) {
      if (append_pending_output(session, buffer, (size_t)size) == -1 ||
          flush_pending_data(session->client_fd, session->pending_output,
                             &session->pending_output_size) == -1) {
        close_attached_client(session);
      }
    }
    return;
  }

  if (size == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return;

  if (size == 0 || errno == EIO) {
    close(session->master_fd);
    session->master_fd = -1;
    close_attached_client(session);
    return;
  }
}

static void drain_attached_input(struct ptyterm_session *session) {
  char buffer[1024];
  ssize_t size;

  if (session->client_fd < 0)
    return;

  if (session->pending_input_size > 0) {
    if (session->master_fd < 0 ||
        flush_pending_data(session->master_fd, session->pending_input,
                           &session->pending_input_size) == -1) {
      close_attached_client(session);
    }
    if (session->pending_input_size > 0)
      return;
  }

  size = read(session->client_fd, buffer, sizeof(buffer));
  if (size > 0) {
    if (session->master_fd < 0 ||
        append_pending_data(&session->pending_input, &session->pending_input_size,
                            &session->pending_input_capacity, buffer,
                            (size_t)size) == -1 ||
        flush_pending_data(session->master_fd, session->pending_input,
                           &session->pending_input_size) == -1) {
      close_attached_client(session);
    }
    return;
  }

  if (size == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return;

  if (size == 0 || errno != EINTR)
    close_attached_client(session);
}

static void cleanup_state(struct ptyterm_daemon_state *state) {
  size_t i;

  for (i = 0; i < state->session_count; ++i) {
    if (state->sessions[i].master_fd >= 0)
      close(state->sessions[i].master_fd);
    if (state->sessions[i].client_fd >= 0)
      close(state->sessions[i].client_fd);
    free(state->sessions[i].pending_input);
    free(state->sessions[i].pending_output);
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

static int send_send_response(int client_fd, struct ptyterm_session *session,
                              uint32_t requested_bytes, uint32_t sent_bytes,
                              uint32_t blocked, const char *reason,
                              uint64_t queue_offset) {
  struct ptyterm_send_response response;

  memset(&response, 0, sizeof(response));
  response.queue_offset = queue_offset;
  response.requested_bytes = requested_bytes;
  response.sent_bytes = sent_bytes;
  response.unsent_bytes = requested_bytes - sent_bytes;
  response.resume_offset = sent_bytes;
  response.blocked = blocked;
  snprintf(response.reason, sizeof(response.reason), "%s", reason);
  session->send_stream_offset += sent_bytes;
  return ptyterm_send_message(client_fd, PTYTERM_MESSAGE_SEND_RESPONSE,
                              &response, sizeof(response));
}

static int handle_send_request(int client_fd, struct ptyterm_daemon_state *state,
                               const void *payload, size_t payload_size) {
  const struct ptyterm_send_request *request;
  struct ptyterm_session *session;
  const char *data;
  ssize_t sent_bytes;
  uint64_t queue_offset;

  if (payload_size < sizeof(*request)) {
    errno = EPROTO;
    return -1;
  }

  request = (const struct ptyterm_send_request *)payload;
  if (payload_size != sizeof(*request) + request->data_size) {
    errno = EPROTO;
    return -1;
  }

  session = (struct ptyterm_session *)find_session(state, request->session_id);
  if (session == NULL) {
    errno = ENOENT;
    return -1;
  }
  if (session->master_fd < 0 || session->state == PTYTERM_SESSION_EXITED) {
    errno = EPIPE;
    return -1;
  }

  data = (const char *)(request + 1);
  queue_offset = session->send_stream_offset;
  sent_bytes = write(session->master_fd, data, request->data_size);
  if (sent_bytes == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return send_send_response(client_fd, session, request->data_size, 0, 1,
                                "would_block", queue_offset);
    }
    return -1;
  }

  return send_send_response(client_fd, session, request->data_size,
                            (uint32_t)sent_bytes,
                            sent_bytes < (ssize_t)request->data_size, 
                            sent_bytes < (ssize_t)request->data_size ?
                                "would_block" : "ok",
                            queue_offset);
}

static int handle_recv_request(int client_fd, struct ptyterm_daemon_state *state,
                               const void *payload, size_t payload_size) {
  const struct ptyterm_recv_request *request;
  struct ptyterm_session *session;
  struct ptyterm_recv_response *response;
  size_t response_size;
  size_t returned_bytes;
  uint64_t start_offset;
  uint64_t oldest_offset;
  char *data;

  if (payload_size != sizeof(*request)) {
    errno = EPROTO;
    return -1;
  }

  request = (const struct ptyterm_recv_request *)payload;
  session = (struct ptyterm_session *)find_session(state, request->session_id);
  if (session == NULL) {
    errno = ENOENT;
    return -1;
  }

  oldest_offset = oldest_available_offset(session);
  start_offset = session->recv_offset;
  if (start_offset < oldest_offset)
    start_offset = oldest_offset;

  response_size = sizeof(*response) + request->max_bytes;
  response = calloc(1, response_size);
  if (response == NULL)
    return -1;

  data = (char *)(response + 1);
  returned_bytes = copy_output_from_offset(session, start_offset, data,
                                           request->max_bytes);
  response->start_offset = start_offset;
  response->oldest_available_offset = oldest_offset;
  response->returned_bytes = (uint32_t)returned_bytes;
  response->end_offset = start_offset + returned_bytes;
  response->next_recv_offset = response->end_offset;
  response->truncated = session->recv_offset < oldest_offset;
  snprintf(response->reason, sizeof(response->reason), "%s",
           returned_bytes == request->max_bytes ? "size_reached" :
           (session->state == PTYTERM_SESSION_EXITED ? "session_exited" :
                                                     (response->truncated ? "truncated_gap" : "ok")));
  session->recv_offset = response->next_recv_offset;

  returned_bytes = ptyterm_send_message(client_fd, PTYTERM_MESSAGE_RECV_RESPONSE,
                                        response,
                                        (uint32_t)(sizeof(*response) +
                                                   response->returned_bytes));
  free(response);
  return (int)returned_bytes;
}

static int handle_attach_request(int client_fd, struct ptyterm_daemon_state *state,
                                 const void *payload, size_t payload_size) {
  const struct ptyterm_attach_request *request;
  struct ptyterm_session *session;
  struct ptyterm_attach_response response;

  if (payload_size != sizeof(*request)) {
    errno = EPROTO;
    return -1;
  }

  request = (const struct ptyterm_attach_request *)payload;
  session = (struct ptyterm_session *)find_session(state, request->session_id);
  if (session == NULL) {
    errno = ENOENT;
    return -1;
  }
  if (session->state == PTYTERM_SESSION_EXITED || session->master_fd < 0) {
    errno = EPIPE;
    return -1;
  }
  if (session->client_fd >= 0 || session->state == PTYTERM_SESSION_ATTACHED) {
    errno = EBUSY;
    return -1;
  }

  memset(&response, 0, sizeof(response));
  response.session_id = session->id;
  response.state = PTYTERM_SESSION_ATTACHED;
  response.child_pid = session->child_pid;
  if (ptyterm_send_message(client_fd, PTYTERM_MESSAGE_ATTACH_RESPONSE,
                           &response, sizeof(response)) == -1) {
    return -1;
  }
  if (set_nonblocking(client_fd) == -1)
    return -1;

  session->client_fd = client_fd;
  session->state = PTYTERM_SESSION_ATTACHED;
  return 1;
}

static int handle_detach_request(int client_fd, struct ptyterm_daemon_state *state,
                                 const void *payload, size_t payload_size) {
  const struct ptyterm_detach_request *request;
  struct ptyterm_session *session;
  struct ptyterm_detach_response response;

  if (payload_size != sizeof(*request)) {
    errno = EPROTO;
    return -1;
  }

  request = (const struct ptyterm_detach_request *)payload;
  session = (struct ptyterm_session *)find_session(state, request->session_id);
  if (session == NULL) {
    errno = ENOENT;
    return -1;
  }
  if (session->client_fd < 0 || session->state != PTYTERM_SESSION_ATTACHED) {
    errno = ENOTCONN;
    return -1;
  }

  close_attached_client(session);
  memset(&response, 0, sizeof(response));
  response.session_id = session->id;
  response.state = session->state;
  return ptyterm_send_message(client_fd, PTYTERM_MESSAGE_DETACH_RESPONSE,
                              &response, sizeof(response));
}

static int handle_resize_request(int client_fd, struct ptyterm_daemon_state *state,
                                 const void *payload, size_t payload_size) {
  const struct ptyterm_resize_request *request;
  struct ptyterm_session *session;
  struct ptyterm_resize_response response;

  if (payload_size != sizeof(*request)) {
    errno = EPROTO;
    return -1;
  }

  request = (const struct ptyterm_resize_request *)payload;
  if (request->rows == 0 || request->cols == 0) {
    errno = EINVAL;
    return -1;
  }

  session = (struct ptyterm_session *)find_session(state, request->session_id);
  if (session == NULL) {
    errno = ENOENT;
    return -1;
  }
  if (apply_session_winsize(session, request->rows, request->cols) == -1)
    return -1;

  memset(&response, 0, sizeof(response));
  response.session_id = session->id;
  response.rows = request->rows;
  response.cols = request->cols;
  return ptyterm_send_message(client_fd, PTYTERM_MESSAGE_RESIZE_RESPONSE,
                              &response, sizeof(response));
}

static int handle_daemon_status_request(int client_fd, const void *payload,
                                        size_t payload_size) {
  struct ptyterm_daemon_status_response response;

  (void)payload;
  if (payload_size != 0) {
    errno = EPROTO;
    return -1;
  }

  memset(&response, 0, sizeof(response));
  response.running = 1;
  response.daemon_pid = (int32_t)getpid();
  return ptyterm_send_message(client_fd, PTYTERM_MESSAGE_DAEMON_STATUS_RESPONSE,
                              &response, sizeof(response));
}

static int handle_daemon_shutdown_request(int client_fd, const void *payload,
                                          size_t payload_size) {
  struct ptyterm_daemon_shutdown_response response;

  (void)payload;
  if (payload_size != 0) {
    errno = EPROTO;
    return -1;
  }

  memset(&response, 0, sizeof(response));
  response.stopping = 1;
  response.daemon_pid = (int32_t)getpid();
  if (ptyterm_send_message(client_fd, PTYTERM_MESSAGE_DAEMON_SHUTDOWN_RESPONSE,
                           &response, sizeof(response)) == -1) {
    return -1;
  }

  stop_requested = 1;
  return 0;
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

static int handle_client(int client_fd, struct ptyterm_daemon_state *state) {
  char payload[4096];
  struct ptyterm_message_header header;
  ssize_t payload_size;

  payload_size = ptyterm_recv_message(client_fd, &header, payload, sizeof(payload));
  if (payload_size == -1) {
    send_error_response(client_fd, errno, strerror(errno));
    return 0;
  }

  switch (header.type) {
  case PTYTERM_MESSAGE_LIST_REQUEST: {
    struct ptyterm_list_request request;

    if ((size_t)payload_size != sizeof(request)) {
      send_error_response(client_fd, EPROTO, "invalid list request size");
      return 0;
    }
    memcpy(&request, payload, sizeof(request));
    if (send_list_response(client_fd, state, request.session_id) == -1) {
      send_error_response(client_fd, errno, strerror(errno));
    }
    return 0;
  }
  case PTYTERM_MESSAGE_BUFFER_INFO_REQUEST: {
    struct ptyterm_buffer_info_request request;

    if ((size_t)payload_size != sizeof(request)) {
      send_error_response(client_fd, EPROTO,
                          "invalid buffer-info request size");
      return 0;
    }
    memcpy(&request, payload, sizeof(request));
    if (request.session_id <= 0) {
      send_error_response(client_fd, EINVAL, "invalid session id");
      return 0;
    }
    if (send_buffer_info_response(client_fd, state, request.session_id) == -1) {
      if (errno == ENOENT) {
        send_error_response(client_fd, errno, "session not found");
      } else {
        send_error_response(client_fd, errno, strerror(errno));
      }
    }
    return 0;
  }
  case PTYTERM_MESSAGE_CREATE_REQUEST:
    if (handle_create_request(client_fd, state, payload, (size_t)payload_size) == -1) {
      send_error_response(client_fd, errno, strerror(errno));
    }
    return 0;
  case PTYTERM_MESSAGE_SEND_REQUEST:
    if (handle_send_request(client_fd, state, payload, (size_t)payload_size) == -1) {
      if (errno == ENOENT) {
        send_error_response(client_fd, errno, "session not found");
      } else {
        send_error_response(client_fd, errno, strerror(errno));
      }
    }
    return 0;
  case PTYTERM_MESSAGE_RECV_REQUEST:
    if (handle_recv_request(client_fd, state, payload, (size_t)payload_size) == -1) {
      if (errno == ENOENT) {
        send_error_response(client_fd, errno, "session not found");
      } else {
        send_error_response(client_fd, errno, strerror(errno));
      }
    }
    return 0;
  case PTYTERM_MESSAGE_ATTACH_REQUEST: {
    int attached;

    attached = handle_attach_request(client_fd, state, payload, (size_t)payload_size);
    if (attached == -1) {
      if (errno == ENOENT) {
        send_error_response(client_fd, errno, "session not found");
      } else {
        send_error_response(client_fd, errno, strerror(errno));
      }
      return 0;
    }
    return attached;
  }
  case PTYTERM_MESSAGE_DETACH_REQUEST:
    if (handle_detach_request(client_fd, state, payload, (size_t)payload_size) == -1) {
      if (errno == ENOENT) {
        send_error_response(client_fd, errno, "session not found");
      } else {
        send_error_response(client_fd, errno, strerror(errno));
      }
    }
    return 0;
  case PTYTERM_MESSAGE_RESIZE_REQUEST:
    if (handle_resize_request(client_fd, state, payload, (size_t)payload_size) == -1) {
      if (errno == ENOENT) {
        send_error_response(client_fd, errno, "session not found");
      } else {
        send_error_response(client_fd, errno, strerror(errno));
      }
    }
    return 0;
  case PTYTERM_MESSAGE_DAEMON_STATUS_REQUEST:
    if (handle_daemon_status_request(client_fd, payload,
                                     (size_t)payload_size) == -1) {
      send_error_response(client_fd, errno, strerror(errno));
    }
    return 0;
  case PTYTERM_MESSAGE_DAEMON_SHUTDOWN_REQUEST:
    if (handle_daemon_shutdown_request(client_fd, payload,
                                       (size_t)payload_size) == -1) {
      send_error_response(client_fd, errno, strerror(errno));
    }
    return 0;
  default:
    send_error_response(client_fd, ENOTSUP, "unsupported request type");
    return 0;
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
    fd_set wfds;
    int maxfd;
    size_t i;
    int client_fd;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(state.server_fd, &rfds);
    maxfd = state.server_fd;
    for (i = 0; i < state.session_count; ++i) {
      if (state.sessions[i].master_fd < 0)
        ;
      else {
        if (state.sessions[i].pending_output_size == 0)
          FD_SET(state.sessions[i].master_fd, &rfds);
        if (state.sessions[i].pending_input_size > 0)
          FD_SET(state.sessions[i].master_fd, &wfds);
        if (maxfd < state.sessions[i].master_fd)
          maxfd = state.sessions[i].master_fd;
      }
      if (state.sessions[i].client_fd >= 0) {
        if (state.sessions[i].pending_input_size == 0)
          FD_SET(state.sessions[i].client_fd, &rfds);
        if (state.sessions[i].pending_output_size > 0)
          FD_SET(state.sessions[i].client_fd, &wfds);
        if (maxfd < state.sessions[i].client_fd)
          maxfd = state.sessions[i].client_fd;
      }
    }

    if (select(maxfd + 1, &rfds, &wfds, NULL, NULL) == -1) {
      if (errno == EINTR)
        continue;
      perror("select");
      exit(EXIT_FAILURE);
    }

    for (i = 0; i < state.session_count; ++i) {
      if (state.sessions[i].master_fd >= 0 && state.sessions[i].pending_input_size > 0 &&
          FD_ISSET(state.sessions[i].master_fd, &wfds)) {
        if (flush_pending_data(state.sessions[i].master_fd,
                               state.sessions[i].pending_input,
                               &state.sessions[i].pending_input_size) == -1) {
          close_attached_client(&state.sessions[i]);
        }
      }
      if (state.sessions[i].client_fd >= 0 && state.sessions[i].pending_output_size > 0 &&
          FD_ISSET(state.sessions[i].client_fd, &wfds)) {
        if (flush_pending_data(state.sessions[i].client_fd,
                               state.sessions[i].pending_output,
                               &state.sessions[i].pending_output_size) == -1) {
          close_attached_client(&state.sessions[i]);
        }
      }
      if (state.sessions[i].client_fd >= 0 &&
          FD_ISSET(state.sessions[i].client_fd, &rfds)) {
        drain_attached_input(&state.sessions[i]);
      }
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

    if (!handle_client(client_fd, &state))
      close(client_fd);
  }

  close(state.server_fd);
  state.server_fd = -1;
  cleanup_state(&state);
  cleanup_socket();
  cleanup_socket_path[0] = '\0';
  return EXIT_SUCCESS;
}