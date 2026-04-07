#!/bin/sh
set -eu

out=$(./ptyterm --help-format=yaml 2>&1) || {
  echo "ptyterm --help-format=yaml: expected exit 0" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q '^program: ptyterm$' || {
  echo "ptyterm --help-format=yaml: expected program field" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q '^version: ' || {
  echo "ptyterm --help-format=yaml: expected version field" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q '^discovery:$' || {
  echo "ptyterm --help-format=yaml: expected discovery section" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'structured_help: ".*--help-format=yaml"' || {
  echo "ptyterm --help-format=yaml: expected structured help discovery command" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q '^  management_operations:$' || {
  echo "ptyterm --help-format=yaml: expected management_operations section" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --create' || {
  echo "ptyterm --help-format=yaml: expected create operation entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}
