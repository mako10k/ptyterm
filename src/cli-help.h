#ifndef PTYTERM_CLI_HELP_H
#define PTYTERM_CLI_HELP_H

#include <stdio.h>

struct ptyterm_cli_help_option {
  const char *long_name;
  const char *short_name;
  const char *argument;
  const char *default_value;
  const char *requires;
  const char *aliases;
  const char *label;
  const char *description;
};

void ptyterm_cli_help_print_usage_section(
    FILE *out, const char *program_name,
    const char *const usage_formats[]);
void ptyterm_cli_help_print_option_section(
    FILE *out, const char *title,
    const struct ptyterm_cli_help_option *options);
void ptyterm_cli_help_print_note_section(
    FILE *out, const char *title, const char *const notes[]);
void ptyterm_cli_help_print_detail_line(FILE *out, const char *line);
void ptyterm_cli_help_print_yaml_quoted(FILE *out, const char *value);

#endif
