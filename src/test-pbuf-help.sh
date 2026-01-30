#!/bin/sh
set -eu

out=$(./pbuf -h 2>&1) || {
  echo "pbuf -h: expected exit 0" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q "Usage" || {
  echo "pbuf -h: expected Usage in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}
