# Project Guidelines

## PMO Agent Discipline

- A custom agent named `PMO` lives under `.github/agents/pmo.agent.md`.
- At the start of every turn that involves repository work, invoke the `PMO` subagent with a short summary of the task, intended next action, and current status.
- At the end of every turn that involves repository work, invoke the `PMO` subagent again with a short summary of what was completed, what remains, and any blockers or validation results.
- The `PMO` agent may use minimal read/progress tools to consult the current repository rules and task state, but it must not design features, write code, or edit repository files.
- Treat the `PMO` response as a fast process-compliance check: follow its sequencing, validation, and discipline reminders unless they conflict with higher-priority system or developer instructions.
- Keep `PMO` invocations brief and operational; it is a process checker, not a design or implementation agent.

## Build and Test

This repository uses GNU Autotools. Prefer an out-of-tree build to keep generated files isolated:

```sh
./bootstrap.sh
mkdir -p build && cd build
../configure
make
```

In-tree builds are also supported with `./bootstrap.sh && ./configure && make`.

- Run the full test suite with `make check` after building.
- Run a single test with `make -C src check TESTS=test-pbuf-copy.sh`.
- Use `make distcheck` when changing build or packaging behavior.
- There is no separate lint target; compilation is the lint gate because [src/Makefile.am](../src/Makefile.am) enables `-Wall -Werror`.
- Generate `config.h` via `configure` before relying on editor tooling that expects `HAVE_CONFIG_H`.

See [README.md](../README.md) for user-facing build and usage examples.

## Architecture

This project is a small set of standalone C utilities under [src](../src):

- `ptyterm`: interactive PTY wrapper that runs a command on a slave PTY and forwards I/O while mirroring window size changes.
- `ptywrap`: allocates a PTY, prints the slave path, and forks a command with stdio attached to the PTY master.
- `biopen`: bidirectional relay between stdin/stdout and a character device or socket opened read-write.
- `pbuf`: buffered stdin-to-stdout copier built around a ring-buffer style implementation with `readv(2)` and `writev(2)`.

The binaries are intentionally simple and mostly live in one source file each. Keep changes local to the relevant utility unless behavior is genuinely shared.

## Conventions

- Follow the existing Autotools portability pattern: include `config.h` behind `HAVE_CONFIG_H`, and add feature checks in [configure.ac](../configure.ac) instead of ad-hoc platform `#ifdef`s.
- Match the current error-handling style: system call failures are usually fatal in `main()` paths and reported with `perror()` plus `EXIT_FAILURE`.
- Preserve the current I/O model: these tools use `select(2)` with explicit `fd_set` management rather than introducing a different event mechanism.
- Keep `setlocale(LC_ALL, "")` in `main()` functions so help text and diagnostics stay locale-aware.
- Tests in [src](../src) are small POSIX shell scripts that exercise built binaries, so behavior changes should usually be covered by extending or adding a `test-*.sh` script.
- Prefer English for source code, comments, diagnostics, tests, and documentation unless a file already has a different established language.
