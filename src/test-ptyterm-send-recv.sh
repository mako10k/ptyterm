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
stdin_expected=$tmpdir/stdin-expected.bin
stdin_received=$tmpdir/stdin-received.bin
eol_expected=$tmpdir/eol-expected.bin
eol_received=$tmpdir/eol-received.bin
redacted_expected=$tmpdir/redacted-expected.bin
redacted_received=$tmpdir/redacted-received.bin
redacted_snapshot=$tmpdir/redacted-snapshot.out
stale_redaction_expected=$tmpdir/stale-redaction-expected.bin
stale_redaction_received=$tmpdir/stale-redaction-received.bin
line_redaction_received=$tmpdir/line-redaction-received.bin
repeat_redaction_received=$tmpdir/repeat-redaction-received.bin
expired_redaction_received=$tmpdir/expired-redaction-received.bin
expired_redaction_script=$tmpdir/expired-redaction.py
partial_expired_redaction_received=$tmpdir/partial-expired-redaction-received.bin
partial_expired_redaction_script=$tmpdir/partial-expired-redaction.py

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

send_out=$(printf 'STDIN' | ./ptyterm --send-stdin --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --send-stdin: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 5/5 bytes' || {
  echo "ptyterm --send-stdin: expected successful send summary" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

printf 'STDIN' >"$stdin_expected"

./ptyterm --recv --recv-size=5 --session=1 --socket="$sock" >"$stdin_received" 2>"$tmpdir/stdin-recv.err" || {
  echo "ptyterm --recv after --send-stdin: expected success" >&2
  cat "$tmpdir/stdin-recv.err" >&2 || true
  exit 1
}

cmp "$stdin_expected" "$stdin_received" || {
  echo "ptyterm --send-stdin: expected exact stdin bytes" >&2
  od -An -tx1 "$stdin_received" >&2 || true
  cat "$tmpdir/stdin-recv.err" >&2 || true
  exit 1
}

send_out=$(printf 'LINE' | ./ptyterm --send-stdin --send-eol=cr --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --send-stdin --send-eol=cr: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 5/5 bytes' || {
  echo "ptyterm --send-stdin --send-eol=cr: expected sent payload plus CR" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

printf 'LINE\r' >"$eol_expected"

./ptyterm --recv --recv-control=with --recv-size=5 --session=1 --socket="$sock" >"$eol_received" 2>"$tmpdir/eol-recv.err" || {
  echo "ptyterm --recv after --send-eol=cr: expected success" >&2
  cat "$tmpdir/eol-recv.err" >&2 || true
  exit 1
}

cmp "$eol_expected" "$eol_received" || {
  echo "ptyterm --send-eol=cr: expected appended carriage return" >&2
  od -An -tx1 "$eol_received" >&2 || true
  cat "$tmpdir/eol-recv.err" >&2 || true
  exit 1
}

send_out=$(printf 'TopSecret29' | ./ptyterm --send-stdin --redact-sent --session=1 --socket="$sock" 2>&1) || {
  echo "ptyterm --send-stdin --redact-sent: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 11/11 bytes' || {
  echo "ptyterm --send-stdin --redact-sent: expected successful send summary" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

if printf '%s\n' "$send_out" | grep -q 'TopSecret29'; then
  echo "ptyterm --send-stdin --redact-sent: send status exposed secret" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
fi

sleep 1

printf '***********' >"$redacted_expected"

./ptyterm --recv --recv-size=11 --session=1 --socket="$sock" >"$redacted_received" 2>"$tmpdir/redacted-recv.err" || {
  echo "ptyterm --recv after --redact-sent: expected success" >&2
  cat "$tmpdir/redacted-recv.err" >&2 || true
  exit 1
}

if grep -q 'TopSecret29' "$redacted_received"; then
  echo "ptyterm --recv after --redact-sent: secret was exposed" >&2
  od -An -tx1 "$redacted_received" >&2 || true
  exit 1
fi

cmp "$redacted_expected" "$redacted_received" || {
  echo "ptyterm --recv after --redact-sent: expected redaction placeholders" >&2
  od -An -tx1 "$redacted_received" >&2 || true
  cat "$tmpdir/redacted-recv.err" >&2 || true
  exit 1
}

./ptyterm --snapshot --session=1 --socket="$sock" >"$redacted_snapshot" || {
  echo "ptyterm --snapshot after --redact-sent: expected success" >&2
  cat "$redacted_snapshot" >&2 || true
  exit 1
}

if grep -q 'TopSecret29' "$redacted_snapshot"; then
  echo "ptyterm --snapshot after --redact-sent: secret was exposed" >&2
  cat "$redacted_snapshot" >&2 || true
  exit 1
fi

stale_create_out=$(./ptyterm --create --socket="$sock" -- /bin/sh -c 'stty raw -echo; dd bs=1 count=10 of=/dev/null 2>/dev/null; exec cat' 2>&1) || {
  echo "ptyterm --create for stale redaction: expected success" >&2
  printf '%s\n' "$stale_create_out" >&2
  exit 1
}

printf '%s\n' "$stale_create_out" | grep -q '^session_id=2$' || {
  echo "ptyterm --create for stale redaction: expected session id 2" >&2
  printf '%s\n' "$stale_create_out" >&2
  exit 1
}

send_out=$(printf 'DiscardMe1' | ./ptyterm --send-stdin --redact-sent --session=2 --socket="$sock" 2>&1) || {
  echo "ptyterm --send-stdin --redact-sent for discarded input: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 10/10 bytes' || {
  echo "ptyterm --send-stdin --redact-sent for discarded input: expected successful send summary" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

send_out=$(printf 'EchoSecret' | ./ptyterm --send-stdin --redact-sent --session=2 --socket="$sock" 2>&1) || {
  echo "ptyterm --send-stdin --redact-sent after stale redaction: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 10/10 bytes' || {
  echo "ptyterm --send-stdin --redact-sent after stale redaction: expected successful send summary" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

printf '**********' >"$stale_redaction_expected"

./ptyterm --recv --recv-size=10 --session=2 --socket="$sock" >"$stale_redaction_received" 2>"$tmpdir/stale-redaction-recv.err" || {
  echo "ptyterm --recv after stale redaction: expected success" >&2
  cat "$tmpdir/stale-redaction-recv.err" >&2 || true
  exit 1
}

if grep -q 'EchoSecret' "$stale_redaction_received"; then
  echo "ptyterm --recv after stale redaction: later secret was exposed" >&2
  od -An -tx1 "$stale_redaction_received" >&2 || true
  exit 1
fi

cmp "$stale_redaction_expected" "$stale_redaction_received" || {
  echo "ptyterm --recv after stale redaction: expected redaction placeholders" >&2
  od -An -tx1 "$stale_redaction_received" >&2 || true
  cat "$tmpdir/stale-redaction-recv.err" >&2 || true
  exit 1
}

line_create_out=$(./ptyterm --create --socket="$sock" -- /bin/sh -c 'stty echo onlcr; sleep 2' 2>&1) || {
  echo "ptyterm --create for newline redaction: expected success" >&2
  printf '%s\n' "$line_create_out" >&2
  exit 1
}

printf '%s\n' "$line_create_out" | grep -q '^session_id=3$' || {
  echo "ptyterm --create for newline redaction: expected session id 3" >&2
  printf '%s\n' "$line_create_out" >&2
  exit 1
}

send_out=$(printf 'EchoLine\n' | ./ptyterm --send-stdin --redact-sent --session=3 --socket="$sock" 2>&1) || {
  echo "ptyterm --send-stdin --redact-sent with newline: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 9/9 bytes' || {
  echo "ptyterm --send-stdin --redact-sent with newline: expected successful send summary" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

./ptyterm --recv --recv-control=with --recv-size=10 --session=3 --socket="$sock" >"$line_redaction_received" 2>"$tmpdir/line-redaction-recv.err" || {
  echo "ptyterm --recv after newline redaction: expected success" >&2
  cat "$tmpdir/line-redaction-recv.err" >&2 || true
  exit 1
}

if grep -q 'EchoLine' "$line_redaction_received"; then
  echo "ptyterm --recv after newline redaction: secret was exposed" >&2
  od -An -tx1 "$line_redaction_received" >&2 || true
  exit 1
fi

if ! grep -F -q '********' "$line_redaction_received"; then
  echo "ptyterm --recv after newline redaction: expected redaction placeholders" >&2
  od -An -tx1 "$line_redaction_received" >&2 || true
  cat "$tmpdir/line-redaction-recv.err" >&2 || true
  exit 1
fi

repeat_create_out=$(./ptyterm --create --socket="$sock" -- /bin/sh -c 'stty echo onlcr; exec cat' 2>&1) || {
  echo "ptyterm --create for repeated redaction: expected success" >&2
  printf '%s\n' "$repeat_create_out" >&2
  exit 1
}

printf '%s\n' "$repeat_create_out" | grep -q '^session_id=4$' || {
  echo "ptyterm --create for repeated redaction: expected session id 4" >&2
  printf '%s\n' "$repeat_create_out" >&2
  exit 1
}

send_out=$(printf 'Twice\n' | ./ptyterm --send-stdin --redact-sent --session=4 --socket="$sock" 2>&1) || {
  echo "ptyterm --send-stdin --redact-sent with repeated echo: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 6/6 bytes' || {
  echo "ptyterm --send-stdin --redact-sent with repeated echo: expected successful send summary" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 1

./ptyterm --recv --recv-control=with --recv-size=100 --session=4 --socket="$sock" >"$repeat_redaction_received" 2>"$tmpdir/repeat-redaction-recv.err" || {
  echo "ptyterm --recv after repeated redaction: expected success" >&2
  cat "$tmpdir/repeat-redaction-recv.err" >&2 || true
  exit 1
}

if grep -q 'Twice' "$repeat_redaction_received"; then
  echo "ptyterm --recv after repeated redaction: repeated secret was exposed" >&2
  od -An -tx1 "$repeat_redaction_received" >&2 || true
  exit 1
fi

if ! grep -F -q '*****' "$repeat_redaction_received"; then
  echo "ptyterm --recv after repeated redaction: expected redaction placeholders" >&2
  od -An -tx1 "$repeat_redaction_received" >&2 || true
  cat "$tmpdir/repeat-redaction-recv.err" >&2 || true
  exit 1
fi

cat >"$expired_redaction_script" <<'PY'
import sys

data = sys.stdin.buffer.read(10)
sys.stdout.buffer.write(b"A" * (1024 * 1024 + 16))
sys.stdout.buffer.write(b"VISIBLE" + data + b"DONE")
sys.stdout.flush()
PY

expired_create_out=$(./ptyterm --create --socket="$sock" -- /bin/sh -c 'stty raw -echo; exec python3 "$1"' sh "$expired_redaction_script" 2>&1) || {
  echo "ptyterm --create for expired redaction: expected success" >&2
  printf '%s\n' "$expired_create_out" >&2
  exit 1
}

printf '%s\n' "$expired_create_out" | grep -q '^session_id=5$' || {
  echo "ptyterm --create for expired redaction: expected session id 5" >&2
  printf '%s\n' "$expired_create_out" >&2
  exit 1
}

send_out=$(printf 'LateSecret' | ./ptyterm --send-stdin --redact-sent --session=5 --socket="$sock" 2>&1) || {
  echo "ptyterm --send-stdin --redact-sent before expiry: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 10/10 bytes' || {
  echo "ptyterm --send-stdin --redact-sent before expiry: expected successful send summary" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 3

./ptyterm --recv --recv-control=with --recv-size=4096 --session=5 --socket="$sock" >"$expired_redaction_received" 2>"$tmpdir/expired-redaction-recv.err" || {
  echo "ptyterm --recv after redaction expiry: expected success" >&2
  cat "$tmpdir/expired-redaction-recv.err" >&2 || true
  exit 1
}

if grep -q 'LateSecret' "$expired_redaction_received"; then
  echo "ptyterm --recv after redaction expiry: delayed secret was exposed" >&2
  od -An -tx1 "$expired_redaction_received" >&2 || true
  exit 1
fi

if ! grep -q 'VISIBLE' "$expired_redaction_received" ||
    ! grep -F -q '**********' "$expired_redaction_received" ||
    ! grep -q 'DONE' "$expired_redaction_received"; then
  echo "ptyterm --recv after redaction expiry: expected visible output around redacted delayed secret" >&2
  od -An -tx1 "$expired_redaction_received" >&2 || true
  cat "$tmpdir/expired-redaction-recv.err" >&2 || true
  exit 1
fi

cat >"$partial_expired_redaction_script" <<'PY'
import sys
import time

data = sys.stdin.buffer.read(10)
sys.stdout.buffer.write(b"A" * (1024 * 1024 + 16))
sys.stdout.buffer.write(data[:4])
sys.stdout.flush()
time.sleep(7)
sys.stdout.buffer.write(data[4:] + b"DONE")
sys.stdout.flush()
PY

partial_expired_create_out=$(./ptyterm --create --socket="$sock" -- /bin/sh -c 'stty raw -echo; exec python3 "$1"' sh "$partial_expired_redaction_script" 2>&1) || {
  echo "ptyterm --create for split expired redaction: expected success" >&2
  printf '%s\n' "$partial_expired_create_out" >&2
  exit 1
}

printf '%s\n' "$partial_expired_create_out" | grep -q '^session_id=6$' || {
  echo "ptyterm --create for split expired redaction: expected session id 6" >&2
  printf '%s\n' "$partial_expired_create_out" >&2
  exit 1
}

send_out=$(printf 'SplitEcho!' | ./ptyterm --send-stdin --redact-sent --session=6 --socket="$sock" 2>&1) || {
  echo "ptyterm --send-stdin --redact-sent before split expiry: expected success" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

printf '%s\n' "$send_out" | grep -q 'sent 10/10 bytes' || {
  echo "ptyterm --send-stdin --redact-sent before split expiry: expected successful send summary" >&2
  printf '%s\n' "$send_out" >&2
  exit 1
}

sleep 10

./ptyterm --recv --recv-control=with --recv-size=4096 --session=6 --socket="$sock" >"$partial_expired_redaction_received" 2>"$tmpdir/partial-expired-redaction-recv.err" || {
  echo "ptyterm --recv after split redaction expiry: expected success" >&2
  cat "$tmpdir/partial-expired-redaction-recv.err" >&2 || true
  exit 1
}

if grep -q 'SplitEcho!' "$partial_expired_redaction_received" ||
    grep -q 'tEcho!' "$partial_expired_redaction_received"; then
  echo "ptyterm --recv after split redaction expiry: delayed split secret was exposed" >&2
  od -An -tx1 "$partial_expired_redaction_received" >&2 || true
  exit 1
fi

if ! grep -F -q '**********DONE' "$partial_expired_redaction_received"; then
  echo "ptyterm --recv after split redaction expiry: expected redacted split secret" >&2
  od -An -tx1 "$partial_expired_redaction_received" >&2 || true
  cat "$tmpdir/partial-expired-redaction-recv.err" >&2 || true
  exit 1
fi
