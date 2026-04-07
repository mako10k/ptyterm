#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-attach-nonblock.$$ 
sock=$tmpdir/daemon.sock
fifo=$tmpdir/blocked.out
daemon_pid=
attach_pid=
holder_pid=
list_pid=

cleanup() {
  if [ -n "${list_pid}" ] && kill -0 "$list_pid" 2>/dev/null; then
    kill "$list_pid" 2>/dev/null || true
    wait "$list_pid" 2>/dev/null || true
  fi
  if [ -n "${attach_pid}" ] && kill -0 "$attach_pid" 2>/dev/null; then
    kill "$attach_pid" 2>/dev/null || true
    wait "$attach_pid" 2>/dev/null || true
  fi
  if [ -n "${holder_pid}" ] && kill -0 "$holder_pid" 2>/dev/null; then
    kill "$holder_pid" 2>/dev/null || true
    wait "$holder_pid" 2>/dev/null || true
  fi
  if [ -n "${daemon_pid}" ] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmpdir"
mkfifo "$fifo"

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

create_out=$(./ptyterm --create --socket="$sock" /bin/sh -c 'cat /dev/zero' 2>&1) || {
  echo "ptyterm --create for attach-nonblock: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for attach-nonblock: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

sleep 30 <"$fifo" &
holder_pid=$!

./ptyterm --attach --session=1 --socket="$sock" < /dev/null >"$fifo" 2>"$tmpdir/attach.err" &
attach_pid=$!

sleep 2

./ptyterm --list --socket="$sock" >"$tmpdir/list.out" 2>"$tmpdir/list.err" &
list_pid=$!

i=0
while kill -0 "$list_pid" 2>/dev/null; do
  i=$((i + 1))
  if [ "$i" -ge 5 ]; then
    echo "ptyterm --list blocked while attach consumer was stalled" >&2
    cat "$tmpdir/daemon.err" >&2 || true
    cat "$tmpdir/attach.err" >&2 || true
    exit 1
  fi
  sleep 1
done
wait "$list_pid"
list_pid=

printf '1	attached	' | grep -q -f - "$tmpdir/list.out" || {
  echo "ptyterm --list after blocked attach: expected attached session entry" >&2
  cat "$tmpdir/list.out" >&2 || true
  cat "$tmpdir/list.err" >&2 || true
  exit 1
}