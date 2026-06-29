#!/bin/sh
set -eu

if ./ptyterm --bad-option >out.txt 2>err.txt; then
  echo "ptyterm --bad-option: expected failure" >&2
  exit 1
fi

grep -q 'unknown option or invalid use:' err.txt || {
  echo "ptyterm --bad-option: expected primary error message" >&2
  cat err.txt >&2
  exit 1
}

grep -q -- "See './ptyterm --help' for text help." err.txt || {
  echo "ptyterm --bad-option: expected text help discovery hint" >&2
  cat err.txt >&2
  exit 1
}

grep -q -- "See './ptyterm --help-format=yaml' for structured help." err.txt || {
  echo "ptyterm --bad-option: expected structured help discovery hint" >&2
  cat err.txt >&2
  exit 1
}

if ./ptyterm --session=1 echo hoge >out.txt 2>err.txt; then
  echo "ptyterm --session=1 echo hoge: expected failure" >&2
  exit 1
fi

grep -q 'management-only options require a management operation' err.txt || {
  echo "ptyterm --session=1 echo hoge: expected management-only option error" >&2
  cat err.txt >&2
  exit 1
}

grep -q -- "See './ptyterm --help' for text help." err.txt || {
  echo "ptyterm --session=1 echo hoge: expected text help discovery hint" >&2
  cat err.txt >&2
  exit 1
}

grep -q -- "See './ptyterm --help-format=yaml' for structured help." err.txt || {
  echo "ptyterm --session=1 echo hoge: expected structured help discovery hint" >&2
  cat err.txt >&2
  exit 1
}

if ./ptyterm --redact-sent >out.txt 2>err.txt; then
  echo "ptyterm --redact-sent: expected failure" >&2
  exit 1
fi

grep -q -- '--redact-sent requires --send-stdin' err.txt || {
  echo "ptyterm --redact-sent: expected send-stdin requirement error" >&2
  cat err.txt >&2
  exit 1
}

if ./ptyterm --send-eol=none >out.txt 2>err.txt; then
  echo "ptyterm --send-eol=none: expected failure" >&2
  exit 1
fi

grep -q -- '--send-eol requires --send-stdin' err.txt || {
  echo "ptyterm --send-eol=none: expected send-stdin requirement error" >&2
  cat err.txt >&2
  exit 1
}

if dd if=/dev/zero bs=4096 count=1 2>/dev/null | ./ptyterm --send-stdin --session=1 >out.txt 2>err.txt; then
  echo "ptyterm --send-stdin with oversized stdin: expected failure" >&2
  exit 1
fi

grep -q -- 'stdin too large for --send-stdin' err.txt || {
  echo "ptyterm --send-stdin with oversized stdin: expected stdin size error before daemon connect" >&2
  cat err.txt >&2
  exit 1
}

rm -f out.txt err.txt
