#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}/ptyterm-escape-filter.$$
raw=$tmpdir/raw.bin
escaped_expected=$tmpdir/escaped.expected
escaped_actual=$tmpdir/escaped.actual
decoded=$tmpdir/decoded.bin
roundtrip=$tmpdir/roundtrip.bin

cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmpdir"

printf 'A\n\\\033\001' >"$raw"
printf '%s' 'A\n\\\e\x01' >"$escaped_expected"

./ptyterm --escape <"$raw" >"$escaped_actual" 2>"$tmpdir/escape.err" || {
  echo "ptyterm --escape: expected success" >&2
  cat "$tmpdir/escape.err" >&2 || true
  exit 1
}

cmp "$escaped_expected" "$escaped_actual" || {
  echo "ptyterm --escape: expected canonical escaped output without trailing newline" >&2
  od -An -tx1 "$escaped_actual" >&2 || true
  od -An -tx1 "$escaped_expected" >&2 || true
  exit 1
}

test ! -s "$tmpdir/escape.err" || {
  echo "ptyterm --escape: expected no stderr output" >&2
  cat "$tmpdir/escape.err" >&2 || true
  exit 1
}

./ptyterm --unescape <"$escaped_expected" >"$decoded" 2>"$tmpdir/unescape.err" || {
  echo "ptyterm --unescape: expected success" >&2
  cat "$tmpdir/unescape.err" >&2 || true
  exit 1
}

cmp "$raw" "$decoded" || {
  echo "ptyterm --unescape: expected decoded bytes to match original" >&2
  od -An -tx1 "$decoded" >&2 || true
  od -An -tx1 "$raw" >&2 || true
  exit 1
}

./ptyterm --unescape <"$escaped_actual" >"$roundtrip" 2>"$tmpdir/roundtrip.err" || {
  echo "ptyterm round-trip: expected success" >&2
  cat "$tmpdir/roundtrip.err" >&2 || true
  exit 1
}

cmp "$raw" "$roundtrip" || {
  echo "ptyterm round-trip: expected original bytes after escape and unescape" >&2
  od -An -tx1 "$roundtrip" >&2 || true
  od -An -tx1 "$raw" >&2 || true
  exit 1
}

printf '%s' '\xZZ' >"$tmpdir/bad.txt"
if ./ptyterm --unescape <"$tmpdir/bad.txt" >"$tmpdir/bad.out" 2>"$tmpdir/bad.err"; then
  echo "ptyterm --unescape invalid input: expected failure" >&2
  exit 1
fi

grep -q 'invalid hex escape in --unescape' "$tmpdir/bad.err" || {
  echo "ptyterm --unescape invalid input: expected hex escape error" >&2
  cat "$tmpdir/bad.err" >&2 || true
  exit 1
}