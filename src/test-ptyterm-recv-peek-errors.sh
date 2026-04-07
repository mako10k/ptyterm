#!/bin/sh
set -eu

if ./ptyterm --peek --session=1 >out.txt 2>err.txt; then
  echo "ptyterm --peek without --recv: expected failure" >&2
  exit 1
fi

grep -q -- '--peek requires --recv' err.txt || {
  echo "ptyterm --peek without --recv: expected validation error" >&2
  cat err.txt >&2
  exit 1
}

grep -q -- '--help-format=yaml' err.txt || {
  echo "ptyterm --peek without --recv: expected structured help discovery hint" >&2
  cat err.txt >&2
  exit 1
}

rm -f out.txt err.txt