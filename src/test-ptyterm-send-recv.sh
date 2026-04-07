#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-send-recv.$$
sock=$tmpdir/daemon.sock
daemon_pid=
expected=$tmpdir/expected.bin
received=$tmpdir/received.bin
received2=$tmpdir/received2.bin
peeked=$tmpdir/peeked.bin
received3=$tmpdir/received3.bin

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
  echo "ptyterm --create for send/recv: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for send/recv: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

sleep 1

send_out=$(./ptyterm --send='ABC' --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --send: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 3/3 bytes' || {
  echo "ptyterm --send: expected successful send summary" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

printf 'ABC' >"$expected"

./ptyterm --recv --recv-size=3 --session=1 --socket="$sock" >"$received" 2>"$tmpdir/recv.err" || {
  echo "ptyterm --recv: expected success" >&2
  cat "$tmpdir/recv.err" >&2 || true
  exit 1
}

cmp "$expected" "$received" || {
  echo "ptyterm --recv: expected exact byte payload" >&2
  od -An -tx1 "$received" >&2 || true
  cat "$tmpdir/recv.err" >&2 || true
  exit 1
}

grep -q 'recv ' "$tmpdir/recv.err" || {
  echo "ptyterm --recv: expected status output on stderr" >&2
  cat "$tmpdir/recv.err" >&2 || true
  exit 1
}

send_out=$(./ptyterm --send='ABC' --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm second --send: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

./ptyterm --recv --peek --recv-size=3 --session=1 --socket="$sock" >"$peeked" 2>"$tmpdir/peek.err" || {
  echo "ptyterm --recv --peek: expected success" >&2
  cat "$tmpdir/peek.err" >&2 || true
  exit 1
}

cmp "$expected" "$peeked" || {
  echo "ptyterm --recv --peek: expected exact byte payload" >&2
  od -An -tx1 "$peeked" >&2 || true
  cat "$tmpdir/peek.err" >&2 || true
  exit 1
}

grep -q 'next-offset=3' "$tmpdir/peek.err" || {
  echo "ptyterm --recv --peek: expected unchanged recv cursor" >&2
  cat "$tmpdir/peek.err" >&2 || true
  exit 1
}

./ptyterm --recv --recv-size=3 --session=1 --socket="$sock" >"$received3" 2>"$tmpdir/recv3.err" || {
  echo "ptyterm recv after peek: expected success" >&2
  cat "$tmpdir/recv3.err" >&2 || true
  exit 1
}

cmp "$expected" "$received3" || {
  echo "ptyterm recv after peek: expected same byte payload" >&2
  od -An -tx1 "$received3" >&2 || true
  cat "$tmpdir/recv3.err" >&2 || true
  exit 1
}

grep -q 'next-offset=6' "$tmpdir/recv3.err" || {
  echo "ptyterm recv after peek: expected advanced recv cursor" >&2
  cat "$tmpdir/recv3.err" >&2 || true
  exit 1
}

./ptyterm --recv --recv-size=3 --session=1 --socket="$sock" >"$received2" 2>"$tmpdir/recv2.err" || {
  echo "ptyterm second --recv: expected success" >&2
  cat "$tmpdir/recv2.err" >&2 || true
  exit 1
}

test ! -s "$received2" || {
  echo "ptyterm second --recv: expected empty payload after cursor advance" >&2
  od -An -tx1 "$received2" >&2 || true
  exit 1
}

grep -q 'recv 0 bytes' "$tmpdir/recv2.err" || {
  echo "ptyterm second --recv: expected empty recv status" >&2
  cat "$tmpdir/recv2.err" >&2 || true
  exit 1
}