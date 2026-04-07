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

rm -f out.txt err.txt