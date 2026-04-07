#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-daemon-lifecycle.$$
sock=$tmpdir/daemon.sock

cleanup() {
  ./ptyterm --daemon-stop --socket="$sock" >/dev/null 2>/dev/null || true
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmpdir"

status_out=$(./ptyterm --daemon-status --socket="$sock" 2>&1) || {
  echo "ptyterm --daemon-status before auto-start: expected success" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

printf '%s\n' "$status_out" | grep -q '^running=no$' || {
  echo "ptyterm --daemon-status before auto-start: expected running=no" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

create_out=$(./ptyterm --create --socket="$sock" /bin/sh -c 'sleep 30' 2>&1) || {
  echo "ptyterm --create with auto-start: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create with auto-start: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

status_out=$(./ptyterm --daemon-status --socket="$sock" 2>&1) || {
  echo "ptyterm --daemon-status after auto-start: expected success" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

printf '%s\n' "$status_out" | grep -q '^running=yes$' || {
  echo "ptyterm --daemon-status after auto-start: expected running=yes" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

daemon_pid=$(printf '%s\n' "$status_out" | sed -n 's/^daemon_pid=//p')
case "$daemon_pid" in
  ''|*[!0-9]*)
    echo "ptyterm --daemon-status after auto-start: expected numeric daemon pid" >&2
    printf '%s\n' "$status_out" >&2
    exit 1
    ;;
esac

stop_out=$(./ptyterm --daemon-stop --socket="$sock" 2>&1) || {
  echo "ptyterm --daemon-stop: expected success" >&2
  printf '%s\n' "$stop_out" >&2
  exit 1
}

printf '%s\n' "$stop_out" | grep -q '^stopping=yes$' || {
  echo "ptyterm --daemon-stop: expected stopping=yes" >&2
  printf '%s\n' "$stop_out" >&2
  exit 1
}

i=0
while [ -S "$sock" ]; do
  i=$((i + 1))
  if [ "$i" -ge 20 ]; then
    echo "ptyterm --daemon-stop: daemon socket still exists" >&2
    exit 1
  fi
  sleep 1
done

status_out=$(./ptyterm --daemon-status --socket="$sock" 2>&1) || {
  echo "ptyterm --daemon-status after stop: expected success" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

printf '%s\n' "$status_out" | grep -q '^running=no$' || {
  echo "ptyterm --daemon-status after stop: expected running=no" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}