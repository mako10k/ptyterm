#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-wait-state.$$
sock=$tmpdir/daemon.sock
daemon_pid=
changed_out=$tmpdir/changed.out
fg_out=$tmpdir/fg.out
return_out=$tmpdir/return.out
exit_out=$tmpdir/exit.out
timeout_out=$tmpdir/timeout.out

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

create_changed=$(./ptyterm --create --socket="$sock" /bin/sh -c 'stty raw -echo; exec cat' 2>&1) || {
  echo "ptyterm --create for snapshot wait changed: expected success" >&2
  printf '%s\n' "$create_changed" >&2
  exit 1
}
printf '%s\n' "$create_changed" | grep -q '^session_id=1$' || {
  echo "ptyterm --create for snapshot wait changed: expected session id 1" >&2
  printf '%s\n' "$create_changed" >&2
  exit 1
}

create_shell=$(./ptyterm --create --socket="$sock" /bin/sh 2>&1) || {
  echo "ptyterm --create for snapshot wait shell: expected success" >&2
  printf '%s\n' "$create_shell" >&2
  exit 1
}
printf '%s\n' "$create_shell" | grep -q '^session_id=2$' || {
  echo "ptyterm --create for snapshot wait shell: expected session id 2" >&2
  printf '%s\n' "$create_shell" >&2
  exit 1
}

create_exit=$(./ptyterm --create --socket="$sock" /bin/sh -c 'sleep 1' 2>&1) || {
  echo "ptyterm --create for snapshot wait exit: expected success" >&2
  printf '%s\n' "$create_exit" >&2
  exit 1
}
printf '%s\n' "$create_exit" | grep -q '^session_id=3$' || {
  echo "ptyterm --create for snapshot wait exit: expected session id 3" >&2
  printf '%s\n' "$create_exit" >&2
  exit 1
}

(
  sleep 1
  ./ptyterm --send='HELLO\r\n' --session=1 --socket="$sock" >/dev/null 2>"$tmpdir/send-changed.err"
) &

./ptyterm --wait-state=snapshot-changed --wait-timeout=2s --session=1 --socket="$sock" >"$changed_out" || {
  echo "ptyterm --wait-state=snapshot-changed: expected success" >&2
  cat "$changed_out" >&2 || true
  exit 1
}

grep -q '^wait outcome: matched$' "$changed_out" || {
  echo "ptyterm --wait-state=snapshot-changed: expected matched outcome" >&2
  cat "$changed_out" >&2 || true
  exit 1
}

grep -q '^matched predicate: snapshot_changed$' "$changed_out" || {
  echo "ptyterm --wait-state=snapshot-changed: expected predicate name" >&2
  cat "$changed_out" >&2 || true
  exit 1
}

grep -q '^HELLO$' "$changed_out" || {
  echo "ptyterm --wait-state=snapshot-changed: expected resolving snapshot payload" >&2
  cat "$changed_out" >&2 || true
  exit 1
}

(
  sleep 1
  ./ptyterm --send='sleep 1\n' --session=2 --socket="$sock" >/dev/null 2>"$tmpdir/send-shell.err"
) &

./ptyterm --wait-state=foreground-changed --wait-timeout=3s --status-format=kv --session=2 --socket="$sock" >"$fg_out" || {
  echo "ptyterm --wait-state=foreground-changed: expected success" >&2
  cat "$fg_out" >&2 || true
  exit 1
}

grep -q '^wait_outcome=matched$' "$fg_out" || {
  echo "ptyterm --wait-state=foreground-changed: expected matched outcome" >&2
  cat "$fg_out" >&2 || true
  exit 1
}

grep -q '^matched_predicate=foreground_changed$' "$fg_out" || {
  echo "ptyterm --wait-state=foreground-changed: expected predicate key" >&2
  cat "$fg_out" >&2 || true
  exit 1
}

./ptyterm --wait-state=shell-returned --wait-timeout=3s --status-format=kv --session=2 --socket="$sock" >"$return_out" || {
  echo "ptyterm --wait-state=shell-returned: expected success" >&2
  cat "$return_out" >&2 || true
  exit 1
}

grep -q '^wait_outcome=matched$' "$return_out" || {
  echo "ptyterm --wait-state=shell-returned: expected matched outcome" >&2
  cat "$return_out" >&2 || true
  exit 1
}

grep -q '^matched_predicate=shell_returned$' "$return_out" || {
  echo "ptyterm --wait-state=shell-returned: expected predicate key" >&2
  cat "$return_out" >&2 || true
  exit 1
}

grep -q '^shell_returned=yes$' "$return_out" || {
  echo "ptyterm --wait-state=shell-returned: expected returned shell state" >&2
  cat "$return_out" >&2 || true
  exit 1
}

./ptyterm --wait-state=session-exited --wait-timeout=3s --status-format=kv --session=3 --socket="$sock" >"$exit_out" || {
  echo "ptyterm --wait-state=session-exited: expected success" >&2
  cat "$exit_out" >&2 || true
  exit 1
}

grep -q '^wait_outcome=session_exited$' "$exit_out" || {
  echo "ptyterm --wait-state=session-exited: expected session_exited outcome" >&2
  cat "$exit_out" >&2 || true
  exit 1
}

grep -q '^state=exited$' "$exit_out" || {
  echo "ptyterm --wait-state=session-exited: expected exited state" >&2
  cat "$exit_out" >&2 || true
  exit 1
}

if ./ptyterm --wait-state=snapshot-changed --wait-timeout=200ms --session=1 --socket="$sock" >"$timeout_out"; then
  echo "ptyterm --wait-state timeout: expected failure" >&2
  cat "$timeout_out" >&2 || true
  exit 1
fi

grep -q '^wait outcome: timeout$' "$timeout_out" || {
  echo "ptyterm --wait-state timeout: expected timeout outcome" >&2
  cat "$timeout_out" >&2 || true
  exit 1
}