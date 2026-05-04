#!/bin/sh
set -eu

command -v script >/dev/null 2>&1 || exit 77

tmpdir=${TMPDIR:-/tmp}/ptyterm-recv-format.$$
sock=$tmpdir/daemon.sock
daemon_pid=

cleanup() {
  if [ -n "${daemon_pid}" ] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmpdir"
./ptytermd --socket="$sock" >"$tmpdir/daemon.out" 2>"$tmpdir/daemon.err" &
daemon_pid=$!

i=0
while [ ! -e "$sock" ]; do
  i=$((i + 1))
  if [ "$i" -ge 10 ]; then
    echo "ptytermd did not create socket" >&2
    cat "$tmpdir/daemon.err" >&2 || true
    exit 1
  fi
  sleep 1
done

create_out=$(./ptyterm --create --socket="$sock" /bin/sh -c 'stty raw -echo; exec cat' 2>&1) || {
  echo "ptyterm --create for recv-format: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for recv-format: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

./ptyterm --send='A\n\\\e' --session=1 --socket="$sock" >/dev/null 2>"$tmpdir/send.err" || {
  echo "ptyterm --send for recv-format: expected success" >&2
  cat "$tmpdir/send.err" >&2 || true
  exit 1
}

script -q -c "exec ./ptyterm --recv --recv-size=4 --session=1 --socket='$sock' 2>'$tmpdir/recv.err'" /dev/null >"$tmpdir/tty.out" 2>"$tmpdir/tty.err" || {
  echo "ptyterm --recv on tty: expected success" >&2
  cat "$tmpdir/tty.out" >&2 || true
  cat "$tmpdir/recv.err" >&2 || true
  cat "$tmpdir/tty.err" >&2 || true
  exit 1
}

tr -d '\r' <"$tmpdir/tty.out" >"$tmpdir/tty.norm"
printf '%s\n' 'A\\' >"$tmpdir/expected.out"

cmp "$tmpdir/expected.out" "$tmpdir/tty.norm" || {
  echo "ptyterm --recv on tty: expected escaped payload with control chars removed" >&2
  od -An -tx1 "$tmpdir/tty.norm" >&2 || true
  od -An -tx1 "$tmpdir/expected.out" >&2 || true
  cat "$tmpdir/recv.err" >&2 || true
  cat "$tmpdir/tty.err" >&2 || true
  exit 1
}

grep -q 'recv 4 bytes' "$tmpdir/recv.err" || {
  echo "ptyterm --recv on tty: expected recv status output on stderr" >&2
  cat "$tmpdir/recv.err" >&2 || true
  cat "$tmpdir/tty.err" >&2 || true
  exit 1
}

./ptyterm --send='A\n\\\e' --session=1 --socket="$sock" >/dev/null 2>"$tmpdir/send-with.err" || {
  echo "ptyterm --send for recv-control: expected success" >&2
  cat "$tmpdir/send-with.err" >&2 || true
  exit 1
}

script -q -c "exec ./ptyterm --recv --recv-control=with --recv-size=4 --session=1 --socket='$sock' 2>'$tmpdir/recv-with.err'" /dev/null >"$tmpdir/tty-with.out" 2>"$tmpdir/tty-with.err" || {
  echo "ptyterm --recv --recv-control=with on tty: expected success" >&2
  cat "$tmpdir/tty-with.out" >&2 || true
  cat "$tmpdir/recv-with.err" >&2 || true
  cat "$tmpdir/tty-with.err" >&2 || true
  exit 1
}

tr -d '\r' <"$tmpdir/tty-with.out" >"$tmpdir/tty-with.norm"
printf '%s\n' 'A\n\\\e' >"$tmpdir/expected-with.out"

cmp "$tmpdir/expected-with.out" "$tmpdir/tty-with.norm" || {
  echo "ptyterm --recv --recv-control=with on tty: expected escaped payload including control chars" >&2
  od -An -tx1 "$tmpdir/tty-with.norm" >&2 || true
  od -An -tx1 "$tmpdir/expected-with.out" >&2 || true
  cat "$tmpdir/recv-with.err" >&2 || true
  cat "$tmpdir/tty-with.err" >&2 || true
  exit 1
}