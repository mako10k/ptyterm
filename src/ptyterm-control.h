#ifndef PTYTERM_CONTROL_H
#define PTYTERM_CONTROL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PTYTERM_SOCKET_PATH_MAX 108
#define PTYTERM_COMMAND_MAX 128
#define PTYTERM_ERROR_MESSAGE_MAX 128
#define PTYTERM_REASON_MAX 32
#define PTYTERM_SESSION_ALL (-1)

enum ptyterm_message_type {
  PTYTERM_MESSAGE_LIST_REQUEST = 1,
  PTYTERM_MESSAGE_LIST_RESPONSE = 2,
  PTYTERM_MESSAGE_BUFFER_INFO_REQUEST = 3,
  PTYTERM_MESSAGE_BUFFER_INFO_RESPONSE = 4,
  PTYTERM_MESSAGE_ERROR = 5,
  PTYTERM_MESSAGE_CREATE_REQUEST = 6,
  PTYTERM_MESSAGE_CREATE_RESPONSE = 7,
  PTYTERM_MESSAGE_SEND_REQUEST = 8,
  PTYTERM_MESSAGE_SEND_RESPONSE = 9,
  PTYTERM_MESSAGE_RECV_REQUEST = 10,
  PTYTERM_MESSAGE_RECV_RESPONSE = 11,
  PTYTERM_MESSAGE_ATTACH_REQUEST = 12,
  PTYTERM_MESSAGE_ATTACH_RESPONSE = 13,
  PTYTERM_MESSAGE_DETACH_REQUEST = 14,
  PTYTERM_MESSAGE_DETACH_RESPONSE = 15,
  PTYTERM_MESSAGE_RESIZE_REQUEST = 16,
  PTYTERM_MESSAGE_RESIZE_RESPONSE = 17,
};

enum ptyterm_session_state {
  PTYTERM_SESSION_ATTACHED = 1,
  PTYTERM_SESSION_DETACHED = 2,
  PTYTERM_SESSION_EXITED = 3,
};

struct ptyterm_message_header {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t size;
};

struct ptyterm_list_request {
  int32_t session_id;
};

struct ptyterm_session_summary {
  uint32_t id;
  uint32_t state;
  int32_t child_pid;
  int32_t exit_status;
  char command[PTYTERM_COMMAND_MAX];
};

struct ptyterm_list_response {
  uint32_t session_count;
};

struct ptyterm_buffer_info_request {
  int32_t session_id;
};

struct ptyterm_buffer_info_response {
  uint32_t id;
  uint32_t state;
  uint32_t buffer_capacity;
  uint32_t buffer_used;
  uint32_t dropped_bytes;
  uint32_t paused_on_full;
};

struct ptyterm_create_request {
  uint32_t argc;
};

struct ptyterm_create_response {
  uint32_t session_id;
  uint32_t state;
  int32_t child_pid;
};

struct ptyterm_send_request {
  int32_t session_id;
  uint32_t data_size;
};

struct ptyterm_send_response {
  uint64_t queue_offset;
  uint32_t requested_bytes;
  uint32_t sent_bytes;
  uint32_t unsent_bytes;
  uint32_t resume_offset;
  uint32_t blocked;
  char reason[PTYTERM_REASON_MAX];
};

struct ptyterm_recv_request {
  int32_t session_id;
  uint32_t max_bytes;
};

struct ptyterm_recv_response {
  uint64_t start_offset;
  uint64_t end_offset;
  uint64_t next_recv_offset;
  uint64_t oldest_available_offset;
  uint32_t returned_bytes;
  uint32_t truncated;
  char reason[PTYTERM_REASON_MAX];
};

struct ptyterm_attach_request {
  int32_t session_id;
};

struct ptyterm_attach_response {
  uint32_t session_id;
  uint32_t state;
  int32_t child_pid;
};

struct ptyterm_detach_request {
  int32_t session_id;
};

struct ptyterm_detach_response {
  uint32_t session_id;
  uint32_t state;
};

struct ptyterm_resize_request {
  int32_t session_id;
  uint16_t rows;
  uint16_t cols;
};

struct ptyterm_resize_response {
  uint32_t session_id;
  uint16_t rows;
  uint16_t cols;
};

struct ptyterm_error_response {
  int32_t error_code;
  char message[PTYTERM_ERROR_MESSAGE_MAX];
};

int ptyterm_default_socket_path(char *buffer, size_t buffer_size);
int ptyterm_connect_socket(const char *socket_path);
int ptyterm_bind_listen_socket(const char *socket_path);
int ptyterm_send_message(int fd, uint16_t type, const void *payload,
                         uint32_t payload_size);
ssize_t ptyterm_recv_message(int fd, struct ptyterm_message_header *header,
                             void *payload, size_t payload_capacity);
const char *ptyterm_session_state_name(uint32_t state);

#endif