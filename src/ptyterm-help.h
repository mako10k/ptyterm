#ifndef PTYTERM_HELP_H
#define PTYTERM_HELP_H

#include <stdio.h>

enum ptyterm_help_format {
  PTYTERM_HELP_FORMAT_TEXT = 1,
  PTYTERM_HELP_FORMAT_YAML = 2,
};

void ptyterm_print_help(FILE *out, const char *program_name, int help_format);

#endif
