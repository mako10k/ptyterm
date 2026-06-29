#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define PACKAGE_STRING "ptyterm"
#define PACKAGE_VERSION "unknown"
#endif

#include "ptyterm-help.h"

#include "cli-help.h"

#include <stdio.h>

struct ptyterm_help_section {
  const char *text_title;
  const char *yaml_name;
  const struct ptyterm_cli_help_option *options;
};

struct ptyterm_help_example {
  const char *description;
  const char *command_format;
};

static const char *const ptyterm_usage_lines[] = {
    "%s [options] [ENVNAME=ENVVALUE ..] [cmd [arg ..]]",
    NULL,
};

static const struct ptyterm_cli_help_option ptyterm_run_options[] = {
    {"--cols", "-c", "N", NULL, NULL, NULL, "  -c, --cols=N",
     "set columns"},
    {"--lines", "-l", "N", NULL, NULL, "--rows", "  -l, --lines=N",
     "set lines"},
    {"--stdin", "-i", "FILE", NULL, NULL, "--input",
     "  -i, --stdin=FILE",
     "read from FILE instead of stdin (alias: --input)"},
    {"--stdout", "-o", "FILE", NULL, NULL, "--output",
     "  -o, --stdout=FILE",
     "write to FILE instead of stdout (alias: --output)"},
    {"--stdout-append", "-a", "FILE", NULL, NULL, "--append",
     "  -a, --stdout-append=FILE",
     "append output to FILE instead of stdout (alias: --append)"},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

static const struct ptyterm_cli_help_option ptyterm_management_options[] = {
    {"--attach", "-A", "none", NULL, "--session", NULL,
     "  -A, --attach", "attach to one daemon-managed session"},
    {"--create", "-C", "none", NULL, NULL, NULL, "  -C, --create",
     "create a daemon-managed session"},
    {"--daemon-status", NULL, "none", NULL, NULL, NULL,
     "      --daemon-status", "report whether the daemon is running"},
    {"--daemon-stop", NULL, "none", NULL, NULL, NULL,
     "      --daemon-stop", "request graceful daemon shutdown"},
    {"--status-format", NULL, "text|kv", NULL, NULL, NULL,
     "      --status-format=text|kv", "select structured status output"},
    {"--detach", "-D", "none", NULL, "--session", NULL,
     "  -D, --detach", "detach one attached daemon-managed session"},
    {"--resize", "-R", "none", NULL, "--session, --rows, --cols", NULL,
     "  -R, --resize", "resize one daemon-managed session"},
    {"--list", "-L", "none", NULL, NULL, NULL, "  -L, --list",
     "list daemon-managed sessions"},
    {"--buffer-info", "-B", "none", NULL, "--session", NULL,
     "  -B, --buffer-info", "show buffer state for one session"},
    {"--send", NULL, "DATA", NULL, "--session", NULL,
     "      --send=DATA", "send decoded bytes to one session"},
    {"--send-stdin", NULL, "none", NULL, "--session", NULL,
     "      --send-stdin", "send bytes read from stdin to one session"},
    {"--send-eol", NULL, "none|lf|cr|crlf", "none", "--send-stdin", NULL,
     "      --send-eol=none|lf|cr|crlf",
     "append a line ending after --send-stdin bytes"},
    {"--redact-sent", NULL, "none", NULL, "--send-stdin", NULL,
     "      --redact-sent",
     "redact bytes sent by --send-stdin from echoed output"},
    {"--recv", NULL, "none", NULL, "--session", NULL, "      --recv",
     "receive buffered output from one session"},
    {"--snapshot", NULL, "none", NULL, "--session", NULL,
     "      --snapshot", "show a readable terminal snapshot for one session"},
    {"--view", NULL, "none", NULL, "--session", NULL, "      --view",
     "open a scrollable full-screen terminal snapshot viewer"},
    {"--wait-state", NULL,
     "snapshot-changed|foreground-changed|shell-returned|session-exited|cursor-changed",
     NULL, "--session, --wait-timeout", NULL,
     "      --wait-state=PREDICATE",
     "wait for a state predicate and print the resolving snapshot"},
    {"--wait-timeout", NULL, "DURATION", NULL, "--wait-state", NULL,
     "      --wait-timeout=DURATION",
     "maximum wait time for --wait-state (ms|s)"},
    {"--screen", NULL, "active|main|alt", "active",
     "--snapshot|--view|--wait-state", NULL,
     "      --screen=active|main|alt",
     "select which screen snapshot to inspect (default: active)"},
    {"--recv-format", NULL, "raw|escaped",
     "escaped on TTY stdout, raw otherwise", "--recv", NULL,
     "      --recv-format=raw|escaped",
     "select recv payload rendering (default: escaped on TTY stdout, raw otherwise)"},
    {"--recv-control", NULL, "without|with", "without", "--recv", NULL,
     "      --recv-control=without|with",
     "drop control characters from recv payload unless explicitly kept"},
    {"--recv-size", NULL, "N", "4096", "--recv", NULL,
     "      --recv-size=N", "maximum bytes returned by --recv"},
    {"--recv-timeout", NULL, "DURATION", NULL, "--recv", NULL,
     "      --recv-timeout=DURATION",
     "wait up to DURATION (ms|s) for recv"},
    {"--recv-until", NULL, "STRING", NULL, "--recv", NULL,
     "      --recv-until=STRING",
     "wait until unread output contains STRING"},
    {"--peek", NULL, "none", NULL, "--recv", NULL, "      --peek",
     "inspect buffered output without advancing recv"},
    {"--session", "-S", "ID", NULL, NULL, NULL,
     "  -S, --session=ID",
     "select one session for management operations"},
    {"--rows", NULL, "N", NULL, "--resize", "--lines",
     "      --rows=N", "rows for --resize (alias: --lines)"},
    {"--cols", "-c", "N", NULL, "--resize", NULL,
     "      --cols=N", "cols for --resize"},
    {"--socket", "-s", "PATH", NULL, NULL, NULL,
     "  -s, --socket=PATH", "override daemon control socket path"},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

static const struct ptyterm_cli_help_option ptyterm_filter_options[] = {
    {"--escape", NULL, "none", NULL, NULL, NULL, "      --escape",
     "read stdin and write escaped bytes to stdout"},
    {"--unescape", NULL, "none", NULL, NULL, NULL, "      --unescape",
     "read escaped stdin and write decoded bytes to stdout"},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

static const struct ptyterm_cli_help_option ptyterm_common_options[] = {
    {"--version", "-V", "none", NULL, NULL, NULL, "  -V, --version",
     "print version and exit"},
    {"--help", "-h", "none", NULL, NULL, NULL, "  -h, --help",
     "print this usage and exit"},
    {"--help-format", NULL, "text|yaml", "text", NULL, NULL,
     "      --help-format=text|yaml",
     "print help in the selected format"},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

static const struct ptyterm_help_section ptyterm_text_sections[] = {
    {"Run options", "run_options", ptyterm_run_options},
    {"Management options", "management_operations", ptyterm_management_options},
    {"Filter options", "filter_operations", ptyterm_filter_options},
    {"Common options", "common_options", ptyterm_common_options},
    {NULL, NULL, NULL},
};

static const struct ptyterm_cli_help_option ptyterm_yaml_common_options[] = {
    {"--help", "-h", "none", NULL, NULL, NULL, NULL,
     "Print text help and exit."},
    {"--help-format", NULL, "text|yaml", "text", NULL, NULL, NULL,
     "Print help in the selected format and exit."},
    {"--version", "-V", "none", NULL, NULL, NULL, NULL,
     "Print version and exit."},
    {"--socket", "-s", "PATH", NULL, NULL, NULL, NULL,
     "Override daemon control socket path."},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

static const struct ptyterm_cli_help_option ptyterm_yaml_run_options[] = {
    {"--stdin", "-i", "FILE", NULL, NULL, "[--input]", NULL,
     "Read from FILE instead of stdin."},
    {"--stdout", "-o", "FILE", NULL, NULL, "[--output]", NULL,
     "Write to FILE instead of stdout."},
    {"--stdout-append", "-a", "FILE", NULL, NULL, "[--append]", NULL,
     "Append output to FILE instead of stdout."},
    {"--cols", "-c", "N", NULL, NULL, NULL, NULL,
     "Set initial PTY columns."},
    {"--lines", "-l", "N", NULL, NULL, "[--rows]", NULL,
     "Set initial PTY lines."},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

static const struct ptyterm_cli_help_option ptyterm_yaml_management_options[] = {
    {"--attach", "-A", "none", NULL, "[--session]", NULL, NULL,
     "Attach to one daemon-managed session."},
    {"--create", "-C", "none", NULL, NULL, NULL, NULL,
     "Create a daemon-managed session."},
    {"--daemon-status", NULL, "none", NULL, NULL, NULL, NULL,
     "Report whether the daemon is running."},
    {"--daemon-stop", NULL, "none", NULL, NULL, NULL, NULL,
     "Request graceful daemon shutdown."},
    {"--detach", "-D", "none", NULL, "[--session]", NULL, NULL,
     "Detach one attached daemon-managed session."},
    {"--resize", "-R", "none", NULL, "[--session, --rows, --cols]", NULL,
     NULL, "Resize one daemon-managed session."},
    {"--list", "-L", "none", NULL, NULL, NULL, NULL,
     "List daemon-managed sessions."},
    {"--buffer-info", "-B", "none", NULL, "[--session]", NULL, NULL,
     "Show buffer state for one session."},
    {"--send", NULL, "DATA", NULL, "[--session]", NULL, NULL,
     "Send decoded bytes to one session."},
    {"--send-stdin", NULL, "none", NULL, "[--session]", NULL, NULL,
     "Send bytes read from stdin to one session."},
    {"--send-eol", NULL, "none|lf|cr|crlf", "none", "[--send-stdin]",
     NULL, NULL, "Append a line ending after bytes read by --send-stdin."},
    {"--redact-sent", NULL, "none", NULL, "[--send-stdin]", NULL, NULL,
     "Redact bytes sent by --send-stdin from echoed session output."},
    {"--recv", NULL, "none", NULL, "[--session]", NULL, NULL,
     "Receive buffered output from one session."},
    {"--snapshot", NULL, "none", NULL, "[--session]", NULL, NULL,
     "Show a readable terminal snapshot for one session."},
    {"--view", NULL, "none", NULL, "[--session]", NULL, NULL,
     "Open a scrollable full-screen terminal snapshot viewer."},
    {"--wait-state", NULL,
     "snapshot-changed|foreground-changed|shell-returned|session-exited|cursor-changed",
     NULL, "[--session, --wait-timeout]", NULL, NULL,
     "Wait for the requested state predicate and print the resolving snapshot."},
    {"--wait-timeout", NULL, "DURATION", NULL, "[--wait-state]", NULL, NULL,
     "Maximum wait time for the state predicate."},
    {"--screen", NULL, "active|main|alt", "active",
     "[--snapshot|--view|--wait-state]", NULL, NULL,
     "Select which terminal screen snapshot to inspect."},
    {"--recv-format", NULL, "raw|escaped",
     "escaped on TTY stdout, raw otherwise", "[--recv]", NULL, NULL,
     "Select exact-byte or terminal-safe recv payload rendering."},
    {"--recv-control", NULL, "without|with", "without", "[--recv]", NULL,
     NULL, "Drop control characters from recv payload unless explicitly kept."},
    {"--status-format", NULL, "text|kv", "kv", NULL, NULL, NULL,
     "Select structured status output."},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

static const struct ptyterm_cli_help_option ptyterm_yaml_filter_options[] = {
    {"--escape", NULL, "none", NULL, NULL, NULL, NULL,
     "Read stdin and write canonical escaped byte notation to stdout."},
    {"--unescape", NULL, "none", NULL, NULL, NULL, NULL,
     "Read escaped byte notation from stdin and write decoded bytes to stdout."},
    {"--peek", NULL, "none", NULL, "[--recv]", NULL, NULL,
     "Read buffered output without advancing the recv cursor."},
    {"--recv-size", NULL, "N", "4096", "[--recv]", NULL, NULL,
     "Maximum bytes returned by recv."},
    {"--recv-timeout", NULL, "DURATION", NULL, "[--recv]", NULL, NULL,
     "Wait up to the requested duration for recv data or a pattern match."},
    {"--recv-until", NULL, "STRING", NULL, "[--recv]", NULL, NULL,
     "Return once unread output contains the fixed string."},
    {"--session", "-S", "ID", NULL, NULL, NULL, NULL,
     "Select one session for management operations."},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

static const struct ptyterm_help_section ptyterm_yaml_sections[] = {
    {NULL, "common_options", ptyterm_yaml_common_options},
    {NULL, "run_options", ptyterm_yaml_run_options},
    {NULL, "management_operations", ptyterm_yaml_management_options},
    {NULL, "filter_operations", ptyterm_yaml_filter_options},
    {NULL, NULL, NULL},
};

static const struct ptyterm_help_example ptyterm_text_examples[] = {
    {"Create a managed session",
     "%s --create -- /bin/sh -c 'echo hello; sleep 30'"},
    {"Send input to one managed session", "%s --session=1 --send='echo hello\\n'"},
    {"Send a provider-supplied secret without exposing the secret in argv",
     "secret-provider ROUTER_PASSWORD | %s --session=1 --send-stdin --send-eol=cr --redact-sent"},
    {"Use secdat as an optional stdin secret provider for ptyterm",
     "secdat get ROUTER_PASSWORD --stdout | %s --session=1 --send-stdin --send-eol=cr --redact-sent"},
    {"Receive buffered output from one managed session", "%s --session=1 --recv"},
    {"Show a terminal snapshot for one managed session",
     "%s --session=1 --snapshot"},
    {"Open a terminal snapshot viewer for one managed session",
     "%s --session=1 --view"},
    {"Wait for a terminal state change and print the resolving snapshot",
     "%s --session=1 --wait-state=snapshot-changed --wait-timeout=2s"},
    {"Print text help", "%s --help"},
    {NULL, NULL},
};

static const struct ptyterm_help_example ptyterm_yaml_examples[] = {
    {"Print text help", "%s --help"},
    {"Print structured help", "%s --help-format=yaml"},
    {"Create a managed session",
     "%s --create -- /bin/sh -c 'echo hello; sleep 30'"},
    {"Send input to one managed session", "%s --session=1 --send='echo hello\\n'"},
    {"Send a provider-supplied secret without exposing the secret in argv",
     "secret-provider ROUTER_PASSWORD | %s --session=1 --send-stdin --send-eol=cr --redact-sent"},
    {"Use secdat as an optional stdin secret provider for ptyterm",
     "secdat get ROUTER_PASSWORD --stdout | %s --session=1 --send-stdin --send-eol=cr --redact-sent"},
    {"Receive buffered output from one managed session", "%s --session=1 --recv"},
    {"Show a terminal snapshot for one managed session",
     "%s --session=1 --snapshot"},
    {"Open a terminal snapshot viewer for one managed session",
     "%s --session=1 --view"},
    {"Wait for a terminal state change and print the resolving snapshot",
     "%s --session=1 --wait-state=snapshot-changed --wait-timeout=2s"},
    {NULL, NULL},
};

static const char *const ptyterm_integration_notes[] = {
    "  secdat can be an optional stdin secret provider; ptyterm remains the PTY/session transport.",
    "  The tools are loosely coupled through stdin/stdout and do not require each other.",
    NULL,
};

static const char *const ptyterm_yaml_notes[] = {
    "Exactly one management operation may be selected per invocation.",
    "Management operations cannot be combined with standalone run output redirection except attach.",
    "Status output formatting is controlled separately with --status-format=text|kv.",
    "secdat integration is optional: secdat can provide bytes on stdout while ptyterm transports them to a managed PTY session.",
    NULL,
};

static void print_help_discovery(FILE *out, const char *program_name) {
  fprintf(out, "Structured help:\n");
  fprintf(out, "  %s --help-format=yaml\n", program_name);
  fprintf(out, "\n");
}

static void print_examples(FILE *out, const char *program_name,
                           const struct ptyterm_help_example *examples) {
  size_t index;

  fprintf(out, "Examples:\n");
  for (index = 0; examples[index].command_format != NULL; index += 1) {
    fprintf(out, "  ");
    fprintf(out, examples[index].command_format, program_name);
    fputc('\n', out);
  }
  fputc('\n', out);
}

static void print_text_help(FILE *out, const char *program_name) {
  size_t index;

  fprintf(out, "%s\n", PACKAGE_STRING);
  fprintf(out, "\n");
  ptyterm_cli_help_print_usage_section(out, program_name, ptyterm_usage_lines);
  for (index = 0; ptyterm_text_sections[index].text_title != NULL; index += 1) {
    ptyterm_cli_help_print_option_section(
        out, ptyterm_text_sections[index].text_title,
        ptyterm_text_sections[index].options);
    fputc('\n', out);
  }
  print_examples(out, program_name, ptyterm_text_examples);
  ptyterm_cli_help_print_note_section(out, "Integration notes",
                                      ptyterm_integration_notes);
  fputc('\n', out);
  print_help_discovery(out, program_name);
}

static void print_yaml_field(FILE *out, const char *indent, const char *name,
                             const char *value) {
  fprintf(out, "%s%s: ", indent, name);
  if (value == NULL) {
    fprintf(out, "null\n");
    return;
  }
  fprintf(out, "%s\n", value);
}

static void print_yaml_option(FILE *out,
                              const struct ptyterm_cli_help_option *option) {
  fprintf(out, "    - long: %s\n", option->long_name);
  print_yaml_field(out, "      ", "short", option->short_name);
  if (option->aliases != NULL)
    print_yaml_field(out, "      ", "aliases", option->aliases);
  print_yaml_field(out, "      ", "argument",
                   option->argument != NULL ? option->argument : "none");
  if (option->default_value != NULL)
    print_yaml_field(out, "      ", "default", option->default_value);
  if (option->requires != NULL)
    print_yaml_field(out, "      ", "requires", option->requires);
  fprintf(out, "      description: %s\n", option->description);
}

static void print_yaml_sections(FILE *out) {
  size_t section_index;

  fprintf(out, "sections:\n");
  for (section_index = 0; ptyterm_yaml_sections[section_index].yaml_name != NULL;
       section_index += 1) {
    const struct ptyterm_cli_help_option *options =
        ptyterm_yaml_sections[section_index].options;
    size_t option_index;

    fprintf(out, "  %s:\n", ptyterm_yaml_sections[section_index].yaml_name);
    for (option_index = 0; options[option_index].long_name != NULL;
         option_index += 1) {
      print_yaml_option(out, &options[option_index]);
    }
  }
}

static void print_yaml_examples(FILE *out, const char *program_name) {
  size_t index;

  fprintf(out, "examples:\n");
  for (index = 0; ptyterm_yaml_examples[index].description != NULL;
       index += 1) {
    char command[1024];

    snprintf(command, sizeof(command), ptyterm_yaml_examples[index].command_format,
             program_name);
    fprintf(out, "  - description: %s\n",
            ptyterm_yaml_examples[index].description);
    fprintf(out, "    command: ");
    ptyterm_cli_help_print_yaml_quoted(out, command);
    fputc('\n', out);
  }
}

static void print_yaml_notes(FILE *out) {
  size_t index;

  fprintf(out, "notes:\n");
  for (index = 0; ptyterm_yaml_notes[index] != NULL; index += 1) {
    fprintf(out, "  - ");
    if (index == 3) {
      ptyterm_cli_help_print_yaml_quoted(out, ptyterm_yaml_notes[index]);
    } else {
      fprintf(out, "%s", ptyterm_yaml_notes[index]);
    }
    fputc('\n', out);
  }
}

static void print_yaml_help(FILE *out, const char *program_name) {
  fprintf(out, "program: ptyterm\n");
  fprintf(out, "version: \"%s\"\n", PACKAGE_VERSION);
  fprintf(out, "summary: Interactive PTY wrapper and daemon session client\n");
  fprintf(out, "default_mode: run\n");
  fprintf(out, "discovery:\n");
  fprintf(out, "  text_help: ");
  fprintf(out, "\"%s --help\"\n", program_name);
  fprintf(out, "  structured_help: ");
  fprintf(out, "\"%s --help-format=yaml\"\n", program_name);
  fprintf(out, "usage:\n");
  fprintf(out, "  - ");
  fprintf(out, "\"%s [options] [ENVNAME=ENVVALUE ..] [cmd [arg ..]]\"\n",
          program_name);
  print_yaml_sections(out);
  print_yaml_examples(out, program_name);
  print_yaml_notes(out);
}

void ptyterm_print_help(FILE *out, const char *program_name, int help_format) {
  if (help_format == PTYTERM_HELP_FORMAT_YAML) {
    print_yaml_help(out, program_name);
    return;
  }

  print_text_help(out, program_name);
}
