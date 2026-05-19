#!/bin/sh
set -eu

command -v script >/dev/null 2>&1 || exit 77

tmpdir=${TMPDIR:-/tmp}/ptyterm-view.$$
sock=$tmpdir/daemon.sock
daemon_pid=
view_out=$tmpdir/view.out

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
  echo "ptyterm --create for view: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for view: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

sleep 1

send_out=$(./ptyterm --send='row01\r\nrow02\r\nrow03\r\nrow04\r\n' --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --send for view: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

{ sleep 1; printf '\003'; } |
  script -q -c "stty rows 4 cols 8 raw -echo; exec ./ptyterm --view --session=1 --socket='$sock'" /dev/null >"$view_out" 2>"$tmpdir/view.err" || {
    echo "ptyterm --view under script: expected success" >&2
    cat "$view_out" >&2 || true
    cat "$tmpdir/view.err" >&2 || true
    exit 1
  }

tr -d '\r' <"$view_out" | grep -q 'row01' || {
  echo "ptyterm --view: expected rendered row in captured output" >&2
  cat "$view_out" >&2 || true
  cat "$tmpdir/view.err" >&2 || true
  exit 1
}

[ ! -s "$tmpdir/view.err" ] || {
  echo "ptyterm --view: expected empty stderr" >&2
  cat "$tmpdir/view.err" >&2 || true
  exit 1
}
