#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-status-format.$$ 
sock=$tmpdir/daemon.sock

cleanup() {
  ./ptyterm --daemon-stop --socket="$sock" >/dev/null 2>/dev/null || true
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmpdir"

status_out=$(./ptyterm --daemon-status --status-format=text --socket="$sock" 2>&1) || {
  echo "ptyterm --daemon-status --status-format=text before auto-start: expected success" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

printf '%s\n' "$status_out" | grep -q '^daemon: stopped$' || {
  echo "ptyterm --daemon-status --status-format=text before auto-start: expected daemon: stopped" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

printf '%s\n' "$status_out" | grep -q '^daemon pid: 0$' || {
  echo "ptyterm --daemon-status --status-format=text before auto-start: expected daemon pid: 0" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

create_out=$(./ptyterm --create --status-format=text --socket="$sock" /bin/sh -c 'sleep 30' 2>&1) || {
  echo "ptyterm --create --status-format=text: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session id: 1$' || {
  echo "ptyterm --create --status-format=text: expected session id: 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^state: detached$' || {
  echo "ptyterm --create --status-format=text: expected detached state" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

status_out=$(./ptyterm --daemon-status --status-format=kv --socket="$sock" 2>&1) || {
  echo "ptyterm --daemon-status --status-format=kv after auto-start: expected success" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

printf '%s\n' "$status_out" | grep -q '^running=yes$' || {
  echo "ptyterm --daemon-status --status-format=kv after auto-start: expected running=yes" >&2
  printf '%s\n' "$status_out" >&2
  exit 1
}

stop_out=$(./ptyterm --daemon-stop --status-format=text --socket="$sock" 2>&1) || {
  echo "ptyterm --daemon-stop --status-format=text: expected success" >&2
  printf '%s\n' "$stop_out" >&2
  exit 1
}

printf '%s\n' "$stop_out" | grep -q '^daemon: stopping$' || {
  echo "ptyterm --daemon-stop --status-format=text: expected daemon: stopping" >&2
  printf '%s\n' "$stop_out" >&2
  exit 1
}
