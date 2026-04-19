#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-create.$$
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

create_out=$(./ptyterm --create --socket="$sock" /bin/sh -c 'printf hello; sleep 2' 2>&1) || {
  echo "ptyterm --create: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q "^Next actions:$" || {
  echo "ptyterm --create: expected next actions guidance" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q "Reuse --socket=$sock with the commands below\." || {
  echo "ptyterm --create: expected socket reuse guidance" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q "^  \./ptyterm --session=1 --send='echo hello\\\\n'$" || {
  echo "ptyterm --create: expected send guidance" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^  \./ptyterm --session=1 --recv$' || {
  echo "ptyterm --create: expected recv guidance" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^  \./ptyterm --help$' || {
  echo "ptyterm --create: expected help guidance" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

sleep 1

list_out=$(./ptyterm --list --socket="$sock" 2>&1) || {
  echo "ptyterm --list after create: expected success" >&2
  printf '%s\n' "$list_out" >&2
  exit 1
}

printf '%s\n' "$list_out" | grep -q '^1	detached	' || {
  echo "ptyterm --list after create: expected detached session entry" >&2
  printf '%s\n' "$list_out" >&2
  exit 1
}

printf '%s\n' "$list_out" | awk -F '\t' '
  NF != 7 { exit 1 }
  $1 != "1" || $2 != "detached" { exit 1 }
  $3 !~ /^[1-9][0-9]*$/ { exit 1 }
  $4 !~ /^\/dev\/pts\/[0-9]+$/ { exit 1 }
  $5 !~ /^[1-9][0-9]*$/ { exit 1 }
  $6 != "sleep" { exit 1 }
  $7 !~ /\/bin\/sh -c printf hello; sleep 2/ { exit 1 }
' || {
  echo "ptyterm --list after create: expected pid, tty, fg_pgid, fg_task, and command fields" >&2
  printf '%s\n' "$list_out" >&2
  exit 1
}

buffer_out=$(./ptyterm --buffer-info --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --buffer-info after create: expected success" >&2
  printf '%s\n' "$buffer_out" >&2
  exit 1
}

printf '%s\n' "$buffer_out" | grep -q '^id=1$' || {
  echo "ptyterm --buffer-info after create: expected session id" >&2
  printf '%s\n' "$buffer_out" >&2
  exit 1
}

printf '%s\n' "$buffer_out" | grep -Eq '^buffer_used=[1-9][0-9]*$' || {
  echo "ptyterm --buffer-info after create: expected buffered output" >&2
  printf '%s\n' "$buffer_out" >&2
  exit 1
}