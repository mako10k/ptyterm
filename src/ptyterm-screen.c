#include "ptyterm-screen.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
  PTYTERM_SCREEN_PARSER_TEXT = 0,
  PTYTERM_SCREEN_PARSER_ESC = 1,
  PTYTERM_SCREEN_PARSER_CSI = 2,
  PTYTERM_SCREEN_PARSER_ESC_CHARSET = 3,
};

static size_t screen_cell_count(uint16_t rows, uint16_t cols) {
  return (size_t)rows * (size_t)cols;
}

static struct ptyterm_screen_buffer *selected_buffer(
    struct ptyterm_screen_state *state, uint32_t selector) {
  uint32_t resolved;

  resolved = selector;
  if (resolved == PTYTERM_SCREEN_SELECTOR_ACTIVE)
    resolved = state->active_screen;
  return resolved == PTYTERM_SCREEN_SELECTOR_ALT ? &state->alt_screen
                                                 : &state->main_screen;
}

static const struct ptyterm_screen_buffer *selected_buffer_const(
    const struct ptyterm_screen_state *state, uint32_t selector,
    uint32_t *selected_screen_out) {
  uint32_t resolved;

  resolved = selector;
  if (resolved == PTYTERM_SCREEN_SELECTOR_ACTIVE)
    resolved = state->active_screen;
  if (selected_screen_out != NULL)
    *selected_screen_out = resolved;
  return resolved == PTYTERM_SCREEN_SELECTOR_ALT ? &state->alt_screen
                                                 : &state->main_screen;
}

static void fill_screen(char *cells, uint16_t rows, uint16_t cols) {
  memset(cells, ' ', screen_cell_count(rows, cols));
}

static void clamp_cursor(const struct ptyterm_screen_state *state,
                         struct ptyterm_screen_buffer *buffer) {
  if (buffer->cursor_row >= state->rows)
    buffer->cursor_row = state->rows - 1;
  if (buffer->cursor_col >= state->cols)
    buffer->cursor_col = state->cols - 1;
  if (buffer->saved_row >= state->rows)
    buffer->saved_row = state->rows - 1;
  if (buffer->saved_col >= state->cols)
    buffer->saved_col = state->cols - 1;
}

static void scroll_up(struct ptyterm_screen_state *state,
                      struct ptyterm_screen_buffer *buffer) {
  size_t row_bytes;
  size_t total_bytes;

  row_bytes = state->cols;
  total_bytes = screen_cell_count(state->rows, state->cols);
  if (state->rows <= 1 || row_bytes == 0)
    return;
  memmove(buffer->cells, buffer->cells + row_bytes, total_bytes - row_bytes);
  memset(buffer->cells + total_bytes - row_bytes, ' ', row_bytes);
}

static void scroll_down(struct ptyterm_screen_state *state,
                        struct ptyterm_screen_buffer *buffer) {
  size_t row_bytes;
  size_t total_bytes;

  row_bytes = state->cols;
  total_bytes = screen_cell_count(state->rows, state->cols);
  if (state->rows <= 1 || row_bytes == 0)
    return;
  memmove(buffer->cells + row_bytes, buffer->cells, total_bytes - row_bytes);
  memset(buffer->cells, ' ', row_bytes);
}

static void line_feed(struct ptyterm_screen_state *state,
                      struct ptyterm_screen_buffer *buffer) {
  if (buffer->cursor_row + 1 >= state->rows) {
    scroll_up(state, buffer);
    buffer->cursor_row = state->rows - 1;
    return;
  }
  buffer->cursor_row += 1;
}

static void put_char(struct ptyterm_screen_state *state,
                     struct ptyterm_screen_buffer *buffer, char value) {
  size_t index;

  index = (size_t)buffer->cursor_row * state->cols + buffer->cursor_col;
  buffer->cells[index] = value;
  if (buffer->cursor_col + 1 >= state->cols) {
    buffer->cursor_col = 0;
    line_feed(state, buffer);
    return;
  }
  buffer->cursor_col += 1;
}

static void clear_screen_range(struct ptyterm_screen_state *state,
                               struct ptyterm_screen_buffer *buffer,
                               size_t start, size_t end) {
  size_t total;

  total = screen_cell_count(state->rows, state->cols);
  if (start > total)
    start = total;
  if (end > total)
    end = total;
  if (start >= end)
    return;
  memset(buffer->cells + start, ' ', end - start);
}

static void clear_line_range(struct ptyterm_screen_state *state,
                             struct ptyterm_screen_buffer *buffer,
                             uint16_t row, uint16_t start_col,
                             uint16_t end_col) {
  size_t row_start;

  if (row >= state->rows)
    return;
  if (start_col > state->cols)
    start_col = state->cols;
  if (end_col > state->cols)
    end_col = state->cols;
  if (start_col >= end_col)
    return;

  row_start = (size_t)row * state->cols;
  memset(buffer->cells + row_start + start_col, ' ', end_col - start_col);
}

static int default_param(int value, int fallback) {
  return value < 0 ? fallback : value;
}

static size_t parse_csi_params(const char *buffer, size_t length, int *params,
                               size_t max_params, int *private_mode) {
  size_t count;
  int current;
  size_t i;

  *private_mode = 0;
  count = 0;
  current = -1;
  i = 0;
  if (length > 0 && buffer[0] == '?') {
    *private_mode = 1;
    i = 1;
  }

  for (; i < length; ++i) {
    unsigned char byte;

    byte = (unsigned char)buffer[i];
    if (isdigit(byte)) {
      current = current < 0 ? (byte - '0') : ((current * 10) + (byte - '0'));
      continue;
    }
    if (byte == ';') {
      if (count < max_params)
        params[count++] = current;
      current = -1;
    }
  }

  if (count < max_params)
    params[count++] = current;
  return count;
}

static void switch_alt_screen(struct ptyterm_screen_state *state, int enable) {
  struct ptyterm_screen_buffer *main_buffer;
  struct ptyterm_screen_buffer *alt_buffer;

  main_buffer = &state->main_screen;
  alt_buffer = &state->alt_screen;
  if (enable) {
    main_buffer->saved_row = main_buffer->cursor_row;
    main_buffer->saved_col = main_buffer->cursor_col;
    state->active_screen = PTYTERM_SCREEN_SELECTOR_ALT;
    alt_buffer->cursor_row = 0;
    alt_buffer->cursor_col = 0;
    fill_screen(alt_buffer->cells, state->rows, state->cols);
    return;
  }

  state->active_screen = PTYTERM_SCREEN_SELECTOR_MAIN;
  main_buffer->cursor_row = main_buffer->saved_row;
  main_buffer->cursor_col = main_buffer->saved_col;
  clamp_cursor(state, main_buffer);
}

static void apply_private_mode(struct ptyterm_screen_state *state,
                               int command, int *params, size_t count) {
  size_t i;

  for (i = 0; i < count; ++i) {
    int value;

    value = params[i] < 0 ? 0 : params[i];
    if (value == 25) {
      state->cursor_visible = command == 'h';
      continue;
    }
    if (value == 47 || value == 1047 || value == 1049)
      switch_alt_screen(state, command == 'h');
  }
}

static void execute_csi(struct ptyterm_screen_state *state, int command) {
  struct ptyterm_screen_buffer *buffer;
  int params[8];
  int private_mode;
  size_t count;
  int row;
  int col;

  buffer = selected_buffer(state, PTYTERM_SCREEN_SELECTOR_ACTIVE);
  count = parse_csi_params(state->csi_buffer, state->csi_length, params,
                           sizeof(params) / sizeof(params[0]), &private_mode);

  if (private_mode) {
    apply_private_mode(state, command, params, count);
    return;
  }

  switch (command) {
  case 'A':
    row = buffer->cursor_row - default_param(params[0], 1);
    buffer->cursor_row = row < 0 ? 0 : (uint16_t)row;
    break;
  case 'B':
    row = buffer->cursor_row + default_param(params[0], 1);
    buffer->cursor_row = row >= state->rows ? state->rows - 1 : (uint16_t)row;
    break;
  case 'C':
    col = buffer->cursor_col + default_param(params[0], 1);
    buffer->cursor_col = col >= state->cols ? state->cols - 1 : (uint16_t)col;
    break;
  case 'D':
    col = buffer->cursor_col - default_param(params[0], 1);
    buffer->cursor_col = col < 0 ? 0 : (uint16_t)col;
    break;
  case 'E':
    row = buffer->cursor_row + default_param(params[0], 1);
    buffer->cursor_row = row >= state->rows ? state->rows - 1 : (uint16_t)row;
    buffer->cursor_col = 0;
    break;
  case 'F':
    row = buffer->cursor_row - default_param(params[0], 1);
    buffer->cursor_row = row < 0 ? 0 : (uint16_t)row;
    buffer->cursor_col = 0;
    break;
  case 'G':
    col = default_param(params[0], 1) - 1;
    if (col < 0)
      col = 0;
    buffer->cursor_col = col >= state->cols ? state->cols - 1 : (uint16_t)col;
    break;
  case 'H':
  case 'f':
    row = default_param(params[0], 1) - 1;
    col = count > 1 ? default_param(params[1], 1) - 1 : 0;
    if (row < 0)
      row = 0;
    if (col < 0)
      col = 0;
    buffer->cursor_row = row >= state->rows ? state->rows - 1 : (uint16_t)row;
    buffer->cursor_col = col >= state->cols ? state->cols - 1 : (uint16_t)col;
    break;
  case 'J': {
    size_t cursor_index;
    int mode;

    cursor_index = (size_t)buffer->cursor_row * state->cols + buffer->cursor_col;
    mode = params[0] < 0 ? 0 : params[0];
    if (mode == 1)
      clear_screen_range(state, buffer, 0, cursor_index + 1);
    else if (mode == 2)
      clear_screen_range(state, buffer, 0,
                         screen_cell_count(state->rows, state->cols));
    else
      clear_screen_range(state, buffer, cursor_index,
                         screen_cell_count(state->rows, state->cols));
    break;
  }
  case 'K': {
    int mode;

    mode = params[0] < 0 ? 0 : params[0];
    if (mode == 1)
      clear_line_range(state, buffer, buffer->cursor_row, 0,
                       buffer->cursor_col + 1);
    else if (mode == 2)
      clear_line_range(state, buffer, buffer->cursor_row, 0, state->cols);
    else
      clear_line_range(state, buffer, buffer->cursor_row, buffer->cursor_col,
                       state->cols);
    break;
  }
  case 'd':
    row = default_param(params[0], 1) - 1;
    if (row < 0)
      row = 0;
    buffer->cursor_row = row >= state->rows ? state->rows - 1 : (uint16_t)row;
    break;
  case 'm':
  case 'r':
    break;
  case 's':
    buffer->saved_row = buffer->cursor_row;
    buffer->saved_col = buffer->cursor_col;
    break;
  case 'u':
    buffer->cursor_row = buffer->saved_row;
    buffer->cursor_col = buffer->saved_col;
    clamp_cursor(state, buffer);
    break;
  default:
    break;
  }
}

static void reset_state(struct ptyterm_screen_state *state) {
  fill_screen(state->main_screen.cells, state->rows, state->cols);
  fill_screen(state->alt_screen.cells, state->rows, state->cols);
  state->active_screen = PTYTERM_SCREEN_SELECTOR_MAIN;
  state->cursor_visible = 1;
  state->parser_state = PTYTERM_SCREEN_PARSER_TEXT;
  state->csi_length = 0;
  memset(&state->main_screen.cursor_row, 0,
         sizeof(state->main_screen) - offsetof(struct ptyterm_screen_buffer,
                                               cursor_row));
  memset(&state->alt_screen.cursor_row, 0,
         sizeof(state->alt_screen) - offsetof(struct ptyterm_screen_buffer,
                                              cursor_row));
}

int ptyterm_screen_init(struct ptyterm_screen_state *state, uint16_t rows,
                        uint16_t cols) {
  size_t count;

  memset(state, 0, sizeof(*state));
  if (rows == 0)
    rows = 24;
  if (cols == 0)
    cols = 80;
  count = screen_cell_count(rows, cols);
  state->rows = rows;
  state->cols = cols;
  state->main_screen.cells = malloc(count);
  if (state->main_screen.cells == NULL)
    return -1;
  state->alt_screen.cells = malloc(count);
  if (state->alt_screen.cells == NULL) {
    free(state->main_screen.cells);
    state->main_screen.cells = NULL;
    return -1;
  }
  reset_state(state);
  return 0;
}

void ptyterm_screen_free(struct ptyterm_screen_state *state) {
  free(state->main_screen.cells);
  free(state->alt_screen.cells);
  memset(state, 0, sizeof(*state));
}

int ptyterm_screen_resize(struct ptyterm_screen_state *state, uint16_t rows,
                          uint16_t cols) {
  char *new_main;
  char *new_alt;
  uint16_t copy_rows;
  uint16_t copy_cols;
  uint16_t row;

  if (rows == 0)
    rows = 24;
  if (cols == 0)
    cols = 80;
  if (rows == state->rows && cols == state->cols)
    return 0;

  new_main = malloc(screen_cell_count(rows, cols));
  if (new_main == NULL)
    return -1;
  new_alt = malloc(screen_cell_count(rows, cols));
  if (new_alt == NULL) {
    free(new_main);
    return -1;
  }

  fill_screen(new_main, rows, cols);
  fill_screen(new_alt, rows, cols);
  copy_rows = rows < state->rows ? rows : state->rows;
  copy_cols = cols < state->cols ? cols : state->cols;
  for (row = 0; row < copy_rows; ++row) {
    memcpy(new_main + (size_t)row * cols,
           state->main_screen.cells + (size_t)row * state->cols, copy_cols);
    memcpy(new_alt + (size_t)row * cols,
           state->alt_screen.cells + (size_t)row * state->cols, copy_cols);
  }

  free(state->main_screen.cells);
  free(state->alt_screen.cells);
  state->main_screen.cells = new_main;
  state->alt_screen.cells = new_alt;
  state->rows = rows;
  state->cols = cols;
  clamp_cursor(state, &state->main_screen);
  clamp_cursor(state, &state->alt_screen);
  state->generation += 1;
  return 0;
}

void ptyterm_screen_feed(struct ptyterm_screen_state *state, const char *data,
                         size_t size) {
  size_t i;
  int changed;

  changed = 0;
  for (i = 0; i < size; ++i) {
    unsigned char byte;
    struct ptyterm_screen_buffer *buffer;

    byte = (unsigned char)data[i];
    buffer = selected_buffer(state, PTYTERM_SCREEN_SELECTOR_ACTIVE);

    if (state->parser_state == PTYTERM_SCREEN_PARSER_ESC_CHARSET) {
      state->parser_state = PTYTERM_SCREEN_PARSER_TEXT;
      continue;
    }

    if (state->parser_state == PTYTERM_SCREEN_PARSER_ESC) {
      state->parser_state = PTYTERM_SCREEN_PARSER_TEXT;
      if (byte == '[') {
        state->parser_state = PTYTERM_SCREEN_PARSER_CSI;
        state->csi_length = 0;
        continue;
      }
      if (byte == '(' || byte == ')' || byte == '*' || byte == '+') {
        state->parser_state = PTYTERM_SCREEN_PARSER_ESC_CHARSET;
        continue;
      }
      if (byte == '7') {
        buffer->saved_row = buffer->cursor_row;
        buffer->saved_col = buffer->cursor_col;
        changed = 1;
        continue;
      }
      if (byte == '8') {
        buffer->cursor_row = buffer->saved_row;
        buffer->cursor_col = buffer->saved_col;
        clamp_cursor(state, buffer);
        changed = 1;
        continue;
      }
      if (byte == 'D') {
        line_feed(state, buffer);
        changed = 1;
        continue;
      }
      if (byte == 'E') {
        line_feed(state, buffer);
        buffer->cursor_col = 0;
        changed = 1;
        continue;
      }
      if (byte == 'M') {
        if (buffer->cursor_row == 0) {
          scroll_down(state, buffer);
        } else {
          buffer->cursor_row -= 1;
        }
        changed = 1;
        continue;
      }
      if (byte == 'c') {
        reset_state(state);
        changed = 1;
      }
      continue;
    }

    if (state->parser_state == PTYTERM_SCREEN_PARSER_CSI) {
      if (byte >= 0x40 && byte <= 0x7e) {
        execute_csi(state, byte);
        state->parser_state = PTYTERM_SCREEN_PARSER_TEXT;
        state->csi_length = 0;
        changed = 1;
        continue;
      }
      if (state->csi_length + 1 < sizeof(state->csi_buffer))
        state->csi_buffer[state->csi_length++] = (char)byte;
      continue;
    }

    if (byte == 0x1b) {
      state->parser_state = PTYTERM_SCREEN_PARSER_ESC;
      continue;
    }
    if (byte == '\r') {
      buffer->cursor_col = 0;
      changed = 1;
      continue;
    }
    if (byte == '\n') {
      line_feed(state, buffer);
      changed = 1;
      continue;
    }
    if (byte == '\b') {
      if (buffer->cursor_col > 0)
        buffer->cursor_col -= 1;
      changed = 1;
      continue;
    }
    if (byte == '\t') {
      uint16_t next_col;

      next_col = (uint16_t)(((buffer->cursor_col / 8) + 1) * 8);
      while (buffer->cursor_col < next_col)
        put_char(state, buffer, ' ');
      changed = 1;
      continue;
    }
    if (byte >= 0x20 && byte != 0x7f) {
      put_char(state, buffer, byte <= 0x7e ? (char)byte : '?');
      changed = 1;
    }
  }

  if (changed)
    state->generation += 1;
}

uint16_t ptyterm_screen_rows(const struct ptyterm_screen_state *state) {
  return state->rows;
}

uint16_t ptyterm_screen_cols(const struct ptyterm_screen_state *state) {
  return state->cols;
}

uint64_t ptyterm_screen_generation(const struct ptyterm_screen_state *state) {
  return state->generation;
}

int ptyterm_screen_cursor_visible(const struct ptyterm_screen_state *state) {
  return state->cursor_visible != 0;
}

uint16_t ptyterm_screen_cursor_row(const struct ptyterm_screen_state *state,
                                   uint32_t selector,
                                   uint32_t *selected_screen_out) {
  const struct ptyterm_screen_buffer *buffer;

  buffer = selected_buffer_const(state, selector, selected_screen_out);
  return buffer->cursor_row;
}

uint16_t ptyterm_screen_cursor_col(const struct ptyterm_screen_state *state,
                                   uint32_t selector,
                                   uint32_t *selected_screen_out) {
  const struct ptyterm_screen_buffer *buffer;

  buffer = selected_buffer_const(state, selector, selected_screen_out);
  return buffer->cursor_col;
}

const char *ptyterm_screen_cells(const struct ptyterm_screen_state *state,
                                 uint32_t selector,
                                 uint32_t *selected_screen_out) {
  return selected_buffer_const(state, selector, selected_screen_out)->cells;
}