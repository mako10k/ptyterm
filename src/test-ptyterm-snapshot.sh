#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-snapshot.$$
sock=$tmpdir/daemon.sock
daemon_pid=
snapshot_out=$tmpdir/snapshot.out
alt_out=$tmpdir/alt.out
main_out=$tmpdir/main.out
kv_out=$tmpdir/kv.out

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
  echo "ptyterm --create for snapshot: expected success" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

printf '%s\n' "$create_out" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for snapshot: expected session id 1" >&2
  printf '%s\n' "$create_out" >&2
  exit 1
}

sleep 1

send_out=$(./ptyterm --send='HELLO\r\n' --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --send for snapshot text: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

send_out=$(./ptyterm --send='\e(BASCII\r\n' --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --send for charset designation: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

./ptyterm --snapshot --session=1 --socket="$sock" >"$snapshot_out" || {
  echo "ptyterm --snapshot: expected success" >&2
  cat "$snapshot_out" >&2 || true
  exit 1
}

grep -q '^screen: main$' "$snapshot_out" || {
  echo "ptyterm --snapshot: expected active main screen" >&2
  cat "$snapshot_out" >&2 || true
  exit 1
}

grep -q '^foreground task: cat$' "$snapshot_out" || {
  echo "ptyterm --snapshot: expected foreground task metadata" >&2
  cat "$snapshot_out" >&2 || true
  exit 1
}

grep -q '^HELLO$' "$snapshot_out" || {
  echo "ptyterm --snapshot: expected rendered shell output" >&2
  cat "$snapshot_out" >&2 || true
  exit 1
}

grep -q '^ASCII$' "$snapshot_out" || {
  echo "ptyterm --snapshot: expected charset designation to be consumed" >&2
  cat "$snapshot_out" >&2 || true
  exit 1
}

if grep -q '^BASCII$' "$snapshot_out"; then
  echo "ptyterm --snapshot: unexpected visible charset designation byte" >&2
  cat "$snapshot_out" >&2 || true
  exit 1
fi

send_out=$(./ptyterm --send='\e[?1049hALT\r\n' --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --send for alt screen: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

./ptyterm --snapshot --session=1 --socket="$sock" >"$alt_out" || {
  echo "ptyterm --snapshot on active alt screen: expected success" >&2
  cat "$alt_out" >&2 || true
  exit 1
}

grep -q '^screen: alt$' "$alt_out" || {
  echo "ptyterm --snapshot: expected active alt screen" >&2
  cat "$alt_out" >&2 || true
  exit 1
}

grep -q '^ALT$' "$alt_out" || {
  echo "ptyterm --snapshot: expected alternate screen content" >&2
  cat "$alt_out" >&2 || true
  exit 1
}

send_out=$(./ptyterm --send='\e[?1049l' --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --send for alt screen restore: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

./ptyterm --snapshot --screen=main --session=1 --socket="$sock" >"$main_out" || {
  echo "ptyterm --snapshot --screen=main: expected success" >&2
  cat "$main_out" >&2 || true
  exit 1
}

grep -q '^screen: main$' "$main_out" || {
  echo "ptyterm --snapshot --screen=main: expected main screen selection" >&2
  cat "$main_out" >&2 || true
  exit 1
}

grep -q '^HELLO$' "$main_out" || {
  echo "ptyterm --snapshot --screen=main: expected preserved main screen content" >&2
  cat "$main_out" >&2 || true
  exit 1
}

./ptyterm --snapshot --status-format=kv --screen=main --session=1 --socket="$sock" >"$kv_out" || {
  echo "ptyterm --snapshot --status-format=kv: expected success" >&2
  cat "$kv_out" >&2 || true
  exit 1
}

grep -q '^session_id=1$' "$kv_out" || {
  echo "ptyterm --snapshot --status-format=kv: expected session_id key" >&2
  cat "$kv_out" >&2 || true
  exit 1
}

grep -q '^screen=main$' "$kv_out" || {
  echo "ptyterm --snapshot --status-format=kv: expected screen key" >&2
  cat "$kv_out" >&2 || true
  exit 1
}

grep -q '^foreground_task=cat$' "$kv_out" || {
  echo "ptyterm --snapshot --status-format=kv: expected foreground_task key" >&2
  cat "$kv_out" >&2 || true
  exit 1
}

grep -q '^row_1=HELLO\\x20' "$kv_out" || {
  echo "ptyterm --snapshot --status-format=kv: expected escaped full-width row payload" >&2
  cat "$kv_out" >&2 || true
  exit 1
}