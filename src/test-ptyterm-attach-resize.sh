#!/bin/sh
set -eu

command -v script >/dev/null 2>&1 || exit 77

tmpdir=${TMPDIR:-/tmp}/ptyterm-attach-resize.$$
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

create_out=$(./ptyterm --create --socket="$sock" /bin/sh -c 'sleep 1; stty size; sleep 3' 2>&1) || {
  echo "ptyterm --create for attach-resize: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for attach-resize: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

script -q -c "stty rows 12 cols 34; exec ./ptyterm --attach --session=1 --socket='$sock'" /dev/null >"$tmpdir/attach.out" 2>"$tmpdir/attach.err" || {
  echo "ptyterm --attach under script: expected success" >&2
  cat "$tmpdir/attach.out" >&2 || true
  cat "$tmpdir/attach.err" >&2 || true
  exit 1
}

tr -d '\r' <"$tmpdir/attach.out" | grep -q '^12 34$' || {
  echo "ptyterm --attach: expected initial winsize to be forwarded" >&2
  cat "$tmpdir/attach.out" >&2 || true
  cat "$tmpdir/attach.err" >&2 || true
  exit 1
}