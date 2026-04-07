#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-recv-wait.$$
sock=$tmpdir/daemon.sock
daemon_pid=
received=$tmpdir/received.bin

cleanup() {
  if [ -n "${sender_pid:-}" ] && kill -0 "$sender_pid" 2>/dev/null; then
    wait "$sender_pid" 2>/dev/null || true
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

create_out=$(./ptyterm --create --socket="$sock" /bin/sh -c 'stty raw -echo; exec cat' 2>&1) || {
  echo "ptyterm --create for recv wait: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for recv wait: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

(
  sleep 1
  ./ptyterm --send='login:' --session=1 --socket="$sock" >/dev/null 2>"$tmpdir/sender.err"
) &
sender_pid=$!

./ptyterm --recv --recv-size=16 --recv-timeout=2s --recv-until='login:' --session=1 --socket="$sock" >"$received" 2>"$tmpdir/recv.err" || {
  echo "ptyterm recv wait until login:: expected success" >&2
  cat "$tmpdir/recv.err" >&2 || true
  exit 1
}

printf 'login:' | cmp - "$received" || {
  echo "ptyterm recv wait until login:: expected matched payload" >&2
  od -An -tx1 "$received" >&2 || true
  cat "$tmpdir/recv.err" >&2 || true
  exit 1
}

grep -q 'reason=match_reached' "$tmpdir/recv.err" || {
  echo "ptyterm recv wait until login:: expected match_reached status" >&2
  cat "$tmpdir/recv.err" >&2 || true
  exit 1
}

if ./ptyterm --recv --recv-size=16 --recv-timeout=200ms --recv-until='password:' --session=1 --socket="$sock" >"$tmpdir/timeout.out" 2>"$tmpdir/timeout.err"; then
  echo "ptyterm recv wait timeout: expected failure" >&2
  cat "$tmpdir/timeout.err" >&2 || true
  exit 1
fi

grep -q 'reason=timeout' "$tmpdir/timeout.err" || {
  echo "ptyterm recv wait timeout: expected timeout reason" >&2
  cat "$tmpdir/timeout.err" >&2 || true
  exit 1
}

if ./ptyterm --recv-until='login:' --session=1 --socket="$sock" >"$tmpdir/bad.out" 2>"$tmpdir/bad.err"; then
  echo "ptyterm --recv-until without --recv: expected failure" >&2
  exit 1
fi

grep -q 'recv wait options require --recv' "$tmpdir/bad.err" || {
  echo "ptyterm --recv-until without --recv: expected validation error" >&2
  cat "$tmpdir/bad.err" >&2 || true
  exit 1
}