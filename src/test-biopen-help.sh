#!/bin/sh
set -eu

out=$(./biopen -h 2>&1) || {
  echo "biopen -h: expected exit 0" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q "Usage" || {
  echo "biopen -h: expected Usage in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q "PATH must name a character device or socket" || {
  echo "biopen -h: expected path note in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

version_out=$(./biopen -V 2>&1) || {
  echo "biopen -V: expected exit 0" >&2
  exit 1
}

printf '%s\n' "$version_out" | grep -q "^ptyterm 0\.10\.0$" || {
  echo "biopen -V: expected PACKAGE_STRING output" >&2
  printf '%s\n' "$version_out" >&2
  exit 1
}