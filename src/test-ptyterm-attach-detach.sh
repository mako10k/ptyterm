#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-attach-detach.$$ 
sock=$tmpdir/daemon.sock
daemon_pid=
attach_pid=

cleanup() {
  if [ -n "${attach_pid}" ] && kill -0 "$attach_pid" 2>/dev/null; then
    kill "$attach_pid" 2>/dev/null || true
    wait "$attach_pid" 2>/dev/null || true
  fi
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

create_out=$(./ptyterm --create --socket="$sock" /bin/sh -c 'sleep 1; printf ready\n; sleep 30' 2>&1) || {
  echo "ptyterm --create for attach/detach: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for attach/detach: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

./ptyterm --attach --session=1 --socket="$sock" < /dev/null >"$tmpdir/attach.out" 2>"$tmpdir/attach.err" &
attach_pid=$!

i=0
while ! tr -d '\r' <"$tmpdir/attach.out" | grep -q 'ready'; do
  i=$((i + 1))
  if [ "$i" -ge 10 ]; then
    echo "ptyterm --attach: expected forwarded output" >&2
    cat "$tmpdir/attach.out" >&2 || true
    cat "$tmpdir/attach.err" >&2 || true
    exit 1
  fi
  sleep 1
done

list_out=$(./ptyterm --list --socket="$sock" 2>&1) || {
  echo "ptyterm --list during attach: expected success" >&2
  printf '%s\n' "$list_out" >&2
  exit 1
}

printf '%s\n' "$list_out" | grep -q '^1	attached	' || {
  echo "ptyterm --list during attach: expected attached session entry" >&2
  printf '%s\n' "$list_out" >&2
  exit 1
}

detach_out=$(./ptyterm --detach --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --detach: expected success" >&2
  printf '%s\n' "$detach_out" >&2
  exit 1
}

printf '%s\n' "$detach_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --detach: expected session id output" >&2
  printf '%s\n' "$detach_out" >&2
  exit 1
}

printf '%s\n' "$detach_out" | grep -q '^state=detached$' || {
  echo "ptyterm --detach: expected detached state output" >&2
  printf '%s\n' "$detach_out" >&2
  exit 1
}

wait "$attach_pid"
attach_pid=

list_out=$(./ptyterm --list --socket="$sock" 2>&1) || {
  echo "ptyterm --list after detach: expected success" >&2
  printf '%s\n' "$list_out" >&2
  exit 1
}

printf '%s\n' "$list_out" | grep -q '^1	detached	' || {
  echo "ptyterm --list after detach: expected detached session entry" >&2
  printf '%s\n' "$list_out" >&2
  exit 1
}