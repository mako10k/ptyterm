#include "cli-help.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define PTYTERM_CLI_HELP_DETAIL_COLUMN 32
#define PTYTERM_CLI_HELP_WRAP_WIDTH 96

static void print_spaces(FILE *out, size_t count) {
  while (count > 0) {
    fputc(' ', out);
    count -= 1;
  }
}

static void print_wrapped_text(FILE *out, const char *text, size_t indent) {
  const char *cursor = text;
  size_t column = indent;
  int wrote_word = 0;

  while (*cursor != '\0') {
    const char *word_start;
    size_t word_length;

    while (*cursor != '\0' && isspace((unsigned char)*cursor))
      cursor += 1;
    if (*cursor == '\0')
      break;

    word_start = cursor;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor))
      cursor += 1;
    word_length = (size_t)(cursor - word_start);

    if (wrote_word &&
        column + 1 + word_length > PTYTERM_CLI_HELP_WRAP_WIDTH) {
      fputc('\n', out);
      print_spaces(out, indent);
      column = indent;
      wrote_word = 0;
    }

    if (wrote_word) {
      fputc(' ', out);
      column += 1;
    }
    fwrite(word_start, 1, word_length, out);
    column += word_length;
    wrote_word = 1;
  }

  fputc('\n', out);
}

void ptyterm_cli_help_print_detail_line(FILE *out, const char *line) {
  const char *separator = NULL;
  const char *description = NULL;
  size_t indent = 0;
  size_t label_length = 0;
  size_t line_length;
  size_t label_width;
  const char *cursor;

  if (line == NULL || line[0] == '\0') {
    fputc('\n', out);
    return;
  }

  line_length = strlen(line);
  while (indent < line_length && line[indent] == ' ')
    indent += 1;
  while (line_length > 0 &&
         (line[line_length - 1] == '\n' || line[line_length - 1] == '\r'))
    line_length -= 1;

  cursor = strstr(line + indent, ": ");
  if (cursor != NULL && (size_t)(cursor - line) < line_length) {
    separator = cursor;
    label_length = (size_t)(separator - (line + indent)) + 1;
    description = separator + 2;
  } else {
    size_t index = indent;

    while (index + 1 < line_length) {
      if (line[index] == ' ' && line[index + 1] == ' ') {
        size_t gap_start = index;
        size_t gap_end = index + 2;

        while (gap_end < line_length && line[gap_end] == ' ')
          gap_end += 1;
        if (gap_end < line_length) {
          separator = line + gap_start;
          label_length = gap_start - indent;
          description = line + gap_end;
          break;
        }
      }
      index += 1;
    }
  }

  if (separator == NULL || description == NULL) {
    fwrite(line, 1, line_length, out);
    fputc('\n', out);
    return;
  }

  print_spaces(out, indent);
  fwrite(line + indent, 1, label_length, out);
  label_width = label_length;
  if (indent + label_width + 1 >= PTYTERM_CLI_HELP_DETAIL_COLUMN) {
    fputc('\n', out);
    print_spaces(out, PTYTERM_CLI_HELP_DETAIL_COLUMN);
  } else {
    print_spaces(out, PTYTERM_CLI_HELP_DETAIL_COLUMN - indent - label_width);
  }
  print_wrapped_text(out, description, PTYTERM_CLI_HELP_DETAIL_COLUMN);
}

void ptyterm_cli_help_print_usage_section(
    FILE *out, const char *program_name,
    const char *const usage_formats[]) {
  size_t index;

  fprintf(out, "Usage:\n");
  for (index = 0; usage_formats[index] != NULL; index += 1) {
    fprintf(out, "  ");
    fprintf(out, usage_formats[index], program_name);
    fputc('\n', out);
  }
  fputc('\n', out);
}

void ptyterm_cli_help_print_option_section(
    FILE *out, const char *title,
    const struct ptyterm_cli_help_option *options) {
  size_t index;

  if (title != NULL)
    fprintf(out, "%s:\n", title);
  for (index = 0; options[index].label != NULL; index += 1) {
    char line[512];

    snprintf(line, sizeof(line), "%s: %s", options[index].label,
             options[index].description);
    ptyterm_cli_help_print_detail_line(out, line);
  }
}

void ptyterm_cli_help_print_note_section(
    FILE *out, const char *title, const char *const notes[]) {
  size_t index;

  if (title != NULL)
    fprintf(out, "%s:\n", title);
  for (index = 0; notes[index] != NULL; index += 1) {
    ptyterm_cli_help_print_detail_line(out, notes[index]);
  }
}

void ptyterm_cli_help_print_yaml_quoted(FILE *out, const char *value) {
  const char *cursor = value != NULL ? value : "";

  fputc('"', out);
  while (*cursor != '\0') {
    switch (*cursor) {
    case '\\':
      fputs("\\\\", out);
      break;
    case '"':
      fputs("\\\"", out);
      break;
    case '\n':
      fputs("\\n", out);
      break;
    case '\r':
      fputs("\\r", out);
      break;
    case '\t':
      fputs("\\t", out);
      break;
    default:
      fputc(*cursor, out);
      break;
    }
    cursor += 1;
  }
  fputc('"', out);
}
