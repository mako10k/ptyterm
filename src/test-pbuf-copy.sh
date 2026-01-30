#!/bin/sh
set -eu

tmpdir=${TMPDIR:-/tmp}
infile="$tmpdir/pbuf.in.$$"
outfile="$tmpdir/pbuf.out.$$"

cleanup() {
  rm -f "$infile" "$outfile"
}
trap cleanup EXIT HUP INT TERM

printf 'hello\nworld\n' >"$infile"
./pbuf <"$infile" >"$outfile"

cmp -s "$infile" "$outfile" || {
  echo "pbuf copy: output did not match input" >&2
  exit 1
}
