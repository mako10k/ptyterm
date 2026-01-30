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

Run a command:

```sh
./src/ptyterm -- ls -la
```

Log output to a file:

```sh
./src/ptyterm --output=pty.log -- bash -lc 'echo hello; sleep 1'
```

`pbuf` (copy with a larger buffer):

```sh
cat bigfile | ./src/pbuf --buffer-size=1m > /tmp/out
```
