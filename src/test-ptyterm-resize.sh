#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-resize.$$
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

create_out=$(./ptyterm --create --socket="$sock" /bin/sh -c 'sleep 1; stty size; sleep 1' 2>&1) || {
  echo "ptyterm --create for resize: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for resize: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

resize_out=$(./ptyterm --resize --session=1 --rows=23 --cols=45 --socket="$sock" 2>&1) || {
  echo "ptyterm --resize: expected success" >&2
  printf '%s\n' "$resize_out" >&2
  exit 1
}

printf '%s\n' "$resize_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --resize: expected session id output" >&2
  printf '%s\n' "$resize_out" >&2
  exit 1
}

printf '%s\n' "$resize_out" | grep -q '^rows=23$' || {
  echo "ptyterm --resize: expected rows output" >&2
  printf '%s\n' "$resize_out" >&2
  exit 1
}

printf '%s\n' "$resize_out" | grep -q '^cols=45$' || {
  echo "ptyterm --resize: expected cols output" >&2
  printf '%s\n' "$resize_out" >&2
  exit 1
}

sleep 2

recv_out=$(./ptyterm --recv --recv-size=32 --session=1 --socket="$sock" 2>"$tmpdir/recv.err") || {
  echo "ptyterm --recv after resize: expected success" >&2
  cat "$tmpdir/recv.err" >&2 || true
  exit 1
}

printf '%s' "$recv_out" | tr -d '\r' | grep -q '^23 45$' || {
  echo "ptyterm --recv after resize: expected resized stty output" >&2
  printf '%s\n' "$recv_out" >&2
  cat "$tmpdir/recv.err" >&2 || true
  exit 1
}