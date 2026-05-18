#ifndef PTYTERM_SCREEN_H
#define PTYTERM_SCREEN_H

#include <stddef.h>
#include <stdint.h>

#include "ptyterm-control.h"

struct ptyterm_screen_buffer {
  char *cells;
  uint16_t cursor_row;
  uint16_t cursor_col;
  uint16_t saved_row;
  uint16_t saved_col;
};

struct ptyterm_screen_state {
  uint16_t rows;
  uint16_t cols;
  uint32_t active_screen;
  uint64_t generation;
  uint8_t cursor_visible;
  uint8_t parser_state;
  size_t csi_length;
  char csi_buffer[64];
  struct ptyterm_screen_buffer main_screen;
  struct ptyterm_screen_buffer alt_screen;
};

int ptyterm_screen_init(struct ptyterm_screen_state *state, uint16_t rows,
                        uint16_t cols);
void ptyterm_screen_free(struct ptyterm_screen_state *state);
int ptyterm_screen_resize(struct ptyterm_screen_state *state, uint16_t rows,
                          uint16_t cols);
void ptyterm_screen_feed(struct ptyterm_screen_state *state, const char *data,
                         size_t size);
uint16_t ptyterm_screen_rows(const struct ptyterm_screen_state *state);
uint16_t ptyterm_screen_cols(const struct ptyterm_screen_state *state);
uint64_t ptyterm_screen_generation(const struct ptyterm_screen_state *state);
int ptyterm_screen_cursor_visible(const struct ptyterm_screen_state *state);
uint16_t ptyterm_screen_cursor_row(const struct ptyterm_screen_state *state,
                                   uint32_t selector,
                                   uint32_t *selected_screen_out);
uint16_t ptyterm_screen_cursor_col(const struct ptyterm_screen_state *state,
                                   uint32_t selector,
                                   uint32_t *selected_screen_out);
const char *ptyterm_screen_cells(const struct ptyterm_screen_state *state,
                                 uint32_t selector,
                                 uint32_t *selected_screen_out);

#endif