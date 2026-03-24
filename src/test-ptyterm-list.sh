#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-list.$$
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

out=$(./ptyterm --list --socket="$sock" 2>&1) || {
  echo "ptyterm --list: expected success" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q "no sessions" || {
  echo "ptyterm --list: expected empty session output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}