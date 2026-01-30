# Copilot instructions for ptyterm

## Build / test / lint

This repo uses GNU Autotools (autoconf/automake).

Configure + build (in-tree):

```sh
./bootstrap.sh
./configure
make
```

Out-of-tree build (useful to keep the repo clean):

```sh
./bootstrap.sh
mkdir -p build && cd build
../configure
make
```

Useful targets:

```sh
make clean
make install   # uses standard DESTDIR/prefix from ./configure
```

Tests:

- `make check` runs a small POSIX-sh test suite (see `src/test-*.sh`).
- Run a single test: `make -C src check TESTS=test-pbuf-copy.sh`

Distribution sanity check:

```sh
make distcheck
```

Linting:

- No dedicated lint target; compilation is treated as a lint gate because `src/Makefile.am` builds with `-Wall -Werror`.
- Autotools defines `-DHAVE_CONFIG_H` via `DEFS` during builds; for clangd, `.clangd` also adds it. Generate `config.h` first via `./configure`.

## High-level architecture

This is a small collection of C utilities around PTYs and stream forwarding, built as separate binaries from `src/`:

- **`ptyterm`** (`src/ptyterm.c`): interactive PTY “terminal wrapper”. It creates a PTY master/slave, optionally rewires stdin/stdout/stderr, forwards I/O with `select(2)`, and mirrors window size changes (SIGWINCH) to the slave.
- **`ptywrap`** (`src/ptywrap.c`): prints the allocated PTY slave path (`ptsname`) and forks/execs a command (defaults to `/bin/cat`) with its stdio attached to the PTY master; supports a daemon mode that redirects stderr to `/dev/null`.
- **`biopen`** (`src/biopen.c`): bidirectional relay between stdin/stdout and a character device/socket opened read-write (defaults to `ttyname(stdin)`), using `select(2)` and internal buffers.
- **`pbuf`** (`src/pbuf.c`): buffered copy utility (`stdin` → `stdout`) with selectable buffer size and an internal ring-buffer-like implementation using `readv/writev`.

The build wiring is straightforward: top-level `Makefile.am` builds the `src` subdir; `src/Makefile.am` declares the four programs and their single-file sources.

## Key conventions in this codebase

- **Autotools config header**: Sources typically include `config.h` when `HAVE_CONFIG_H` is defined; keep feature checks and portability bits in `configure.ac`/`config.h` rather than ad-hoc `#ifdef`s.
- **Error handling style**: Most utilities treat system call failures as fatal and immediately `perror()` + `exit(EXIT_FAILURE)` (or return `-1` in library-like helpers such as `pbuf()`).
- **I/O multiplexing**: Data movement is generally done with `select(2)` and explicit FD sets; when extending I/O paths, follow the existing “build FD sets → select → handle one ready case → continue” loop pattern.
- **Locale**: Each `main()` calls `setlocale(LC_ALL, "")`; keep user-facing messages compatible with that assumption.
