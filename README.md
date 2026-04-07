ptyterm
======

Small C utilities around pseudo-terminals (PTYs) and stream forwarding.

Binaries
--------

All binaries are built from `src/`.

- `ptyterm`: Interactive PTY “terminal wrapper” that runs a command (default: `$SHELL`) on a PTY slave and forwards I/O between your terminal and the PTY master.
- `ptywrap`: Allocates a PTY, prints the slave path, and forks/execs a command with stdio attached to the PTY master (default: `/bin/cat`).
- `biopen`: Bidirectional relay between stdin/stdout and a character device or socket opened read-write.
- `pbuf`: Buffered copy utility (`stdin` → `stdout`) with configurable buffer size.

Build
-----

This project uses GNU Autotools.

Dependencies (typical):

- autoconf
- automake
- libtool
- a C compiler (gcc/clang)

In-tree build:

```sh
./bootstrap.sh
./configure
make
```

Out-of-tree build:

```sh
./bootstrap.sh
mkdir -p build && cd build
../configure
make
```

Install:

```sh
./configure --prefix=/usr/local
make
make install
```

Packaging-style install:

```sh
make install DESTDIR=/tmp/pkgroot
```

Tests
-----

```sh
make check
```

Run a single test:

```sh
make -C src check TESTS=test-pbuf-copy.sh
```

Usage examples
--------------

`ptyterm` (interactive):

```sh
./src/ptyterm
```

Daemon-backed management auto-starts `ptytermd` when needed:

```sh
./src/ptyterm --create -- bash -lc 'echo hello; sleep 30'
./src/ptyterm --list
./src/ptyterm --daemon-status
./src/ptyterm --daemon-stop
```

Help and automation discovery:

```sh
./src/ptyterm --help
./src/ptyterm --help-format=yaml
```

The default help stays text-first for terminal use. The YAML form is a stable,
alternate representation intended for automation, prompting, and tool-driven
capability discovery. Argument and option errors point to both help entry
points.

Documentation status
--------------------

- `ptyterm`: README examples, text help, and YAML help are the canonical user-facing documentation.
- `ptytermd`: text help is implemented and README examples below cover daemon startup and configuration.
- `ptywrap`: text help is implemented and README examples below cover both default and daemon modes.
- `pbuf`: text help is implemented and README examples below cover the main buffered-copy workflow.
- `biopen`: no built-in `--help` or `--version` interface is currently implemented; the supported interface is the positional `[PATH]` form described below.

Man-page status
---------------

This release does not ship project man pages for `ptyterm`, `ptytermd`, `ptywrap`, `biopen`, or `pbuf`.
For v0.10.0, the canonical release-facing documentation is the combination of this README and each binary's built-in help output where available. Man-page coverage is explicitly deferred rather than implicit.

Run a command:

```sh
./src/ptyterm -- ls -la
```

Log output to a file:

```sh
./src/ptyterm --output=pty.log -- bash -lc 'echo hello; sleep 1'
```

`ptytermd` (manual daemon startup):

```sh
./src/ptytermd --socket=/tmp/ptyterm.sock --output-buffer=64k
```

`ptywrap` (print the slave PTY path and run the default `/bin/cat`):

```sh
./src/ptywrap
```

`ptywrap` (daemonize the wrapped command):

```sh
./src/ptywrap --daemon -- /bin/sh -c 'echo hello >/tmp/ptywrap.log; sleep 5'
```

`biopen` (bridge stdin/stdout to a tty or socket):

```sh
./src/biopen /dev/ttyUSB0
```

`pbuf` (copy with a larger buffer):

```sh
cat bigfile | ./src/pbuf --buffer-size=1m > /tmp/out
```

