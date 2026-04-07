#include "ptyterm-control.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define PTYTERM_CONTROL_MAGIC 0x50545953u
#define PTYTERM_CONTROL_VERSION 1u

static int fill_sockaddr_un(const char *socket_path, struct sockaddr_un *addr,
                            socklen_t *addrlen) {
  size_t path_length;

  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;

  path_length = strlen(socket_path);
  if (path_length >= sizeof(addr->sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  memcpy(addr->sun_path, socket_path, path_length + 1);
  *addrlen = offsetof(struct sockaddr_un, sun_path) + path_length + 1;
  return 0;
}

static ssize_t read_all(int fd, void *buffer, size_t size) {
  size_t offset;

  offset = 0;
  while (offset < size) {
    ssize_t chunk;

    chunk = read(fd, (char *)buffer + offset, size - offset);
    if (chunk == 0) {
      errno = ECONNRESET;
      return -1;
    }
    if (chunk == -1) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    offset += (size_t)chunk;
  }
  return (ssize_t)offset;
}

static ssize_t write_all(int fd, const void *buffer, size_t size) {
  size_t offset;

  offset = 0;
  while (offset < size) {
    ssize_t chunk;

    chunk = write(fd, (const char *)buffer + offset, size - offset);
    if (chunk == -1) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    offset += (size_t)chunk;
  }
  return (ssize_t)offset;
}

static int discard_bytes(int fd, size_t size) {
  char buffer[256];

  while (size > 0) {
    size_t chunk_size;

    chunk_size = size < sizeof(buffer) ? size : sizeof(buffer);
    if (read_all(fd, buffer, chunk_size) == -1)
      return -1;
    size -= chunk_size;
  }
  return 0;
}

static int ensure_socket_dir(const char *socket_path) {
  char dir_path[PTYTERM_SOCKET_PATH_MAX];
  char *slash;
  struct stat st;

  if (strlen(socket_path) >= sizeof(dir_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  strcpy(dir_path, socket_path);
  slash = strrchr(dir_path, '/');
  if (slash == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (slash == dir_path) {
    return 0;
  }

  *slash = '\0';
  if (stat(dir_path, &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return -1;
    }
    return 0;
  }
  if (errno != ENOENT)
    return -1;
  if (mkdir(dir_path, 0700) == -1)
    return -1;
  return 0;
}

int ptyterm_default_socket_path(char *buffer, size_t buffer_size) {
  const char *runtime_dir;
  int len;

  runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (runtime_dir && *runtime_dir) {
    len = snprintf(buffer, buffer_size, "%s/ptyterm/daemon.sock", runtime_dir);
  } else {
    len = snprintf(buffer, buffer_size, "/tmp/ptyterm-%lu/daemon.sock",
                   (unsigned long)getuid());
  }

  if (len < 0 || (size_t)len >= buffer_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

int ptyterm_connect_socket(const char *socket_path) {
  int fd;
  struct sockaddr_un addr;
  socklen_t addrlen;

  if (fill_sockaddr_un(socket_path, &addr, &addrlen) == -1)
    return -1;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    return -1;

  if (connect(fd, (struct sockaddr *)&addr, addrlen) == -1) {
    close(fd);
    return -1;
  }

  return fd;
}

int ptyterm_bind_listen_socket(const char *socket_path) {
  int fd;
  int probe_fd;
  struct sockaddr_un addr;
  socklen_t addrlen;

  if (ensure_socket_dir(socket_path) == -1)
    return -1;
  if (fill_sockaddr_un(socket_path, &addr, &addrlen) == -1)
    return -1;

  if (access(socket_path, F_OK) == 0) {
    probe_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe_fd == -1)
      return -1;
    if (connect(probe_fd, (struct sockaddr *)&addr, addrlen) == 0) {
      close(probe_fd);
      errno = EADDRINUSE;
      return -1;
    }
    close(probe_fd);
    if (unlink(socket_path) == -1 && errno != ENOENT)
      return -1;
  } else if (errno != ENOENT) {
    return -1;
  }

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    return -1;
  if (bind(fd, (struct sockaddr *)&addr, addrlen) == -1) {
    close(fd);
    return -1;
  }
  if (chmod(socket_path, 0600) == -1) {
    close(fd);
    unlink(socket_path);
    return -1;
  }
  if (listen(fd, 16) == -1) {
    close(fd);
    unlink(socket_path);
    return -1;
  }
  return fd;
}

int ptyterm_send_message(int fd, uint16_t type, const void *payload,
                         uint32_t payload_size) {
  struct ptyterm_message_header header;

  header.magic = PTYTERM_CONTROL_MAGIC;
  header.version = PTYTERM_CONTROL_VERSION;
  header.type = type;
  header.size = payload_size;

  if (write_all(fd, &header, sizeof(header)) == -1)
    return -1;
  if (payload_size > 0 && write_all(fd, payload, payload_size) == -1)
    return -1;
  return 0;
}

ssize_t ptyterm_recv_message(int fd, struct ptyterm_message_header *header,
                             void *payload, size_t payload_capacity) {
  if (read_all(fd, header, sizeof(*header)) == -1)
    return -1;

  if (header->magic != PTYTERM_CONTROL_MAGIC ||
      header->version != PTYTERM_CONTROL_VERSION) {
    errno = EPROTO;
    return -1;
  }

  if (header->size > payload_capacity) {
    if (discard_bytes(fd, header->size) == -1)
      return -1;
    errno = EMSGSIZE;
    return -1;
  }

  if (header->size > 0 && read_all(fd, payload, header->size) == -1)
    return -1;
  return (ssize_t)header->size;
}

const char *ptyterm_session_state_name(uint32_t state) {
  switch (state) {
  case PTYTERM_SESSION_ATTACHED:
    return "attached";
  case PTYTERM_SESSION_DETACHED:
    return "detached";
  case PTYTERM_SESSION_EXITED:
    return "exited";
  default:
    return "unknown";
  }
}