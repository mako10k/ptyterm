#!/bin/sh
set -eu

out=$(./ptyterm -h 2>&1) || {
  echo "ptyterm -h: expected exit 0" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q "Usage" || {
  echo "ptyterm -h: expected Usage in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--stdin=FILE" || {
  echo "ptyterm -h: expected canonical --stdin option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--stdout=FILE" || {
  echo "ptyterm -h: expected canonical --stdout option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--create" || {
  echo "ptyterm -h: expected create option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--daemon-status" || {
  echo "ptyterm -h: expected daemon-status option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--daemon-stop" || {
  echo "ptyterm -h: expected daemon-stop option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--help-format=text|yaml" || {
  echo "ptyterm -h: expected help-format option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "Structured help:" || {
  echo "ptyterm -h: expected structured help discovery section" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--help-format=yaml" || {
  echo "ptyterm -h: expected structured help command in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--status-format=text|kv" || {
  echo "ptyterm -h: expected status-format option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--attach" || {
  echo "ptyterm -h: expected attach option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--detach" || {
  echo "ptyterm -h: expected detach option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--resize" || {
  echo "ptyterm -h: expected resize option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--send=DATA" || {
  echo "ptyterm -h: expected send option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--escape" || {
  echo "ptyterm -h: expected escape option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--unescape" || {
  echo "ptyterm -h: expected unescape option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--recv" || {
  echo "ptyterm -h: expected recv option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--snapshot" || {
  echo "ptyterm -h: expected snapshot option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--view" || {
  echo "ptyterm -h: expected view option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--wait-state=PREDICATE" || {
  echo "ptyterm -h: expected wait-state option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--wait-timeout=DURATION" || {
  echo "ptyterm -h: expected wait-timeout option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--screen=active|main|alt" || {
  echo "ptyterm -h: expected screen option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--recv-format=raw|escaped" || {
  echo "ptyterm -h: expected recv-format option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--recv-timeout=DURATION" || {
  echo "ptyterm -h: expected recv-timeout option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--recv-until=STRING" || {
  echo "ptyterm -h: expected recv-until option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "Examples:" || {
  echo "ptyterm -h: expected examples section" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--session=1 --send='echo hello\\\\n'" || {
  echo "ptyterm -h: expected send example in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--session=1 --recv" || {
  echo "ptyterm -h: expected recv example in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--session=1 --snapshot" || {
  echo "ptyterm -h: expected snapshot example in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--session=1 --view" || {
  echo "ptyterm -h: expected view example in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--session=1 --wait-state=snapshot-changed --wait-timeout=2s" || {
  echo "ptyterm -h: expected wait-state example in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--peek" || {
  echo "ptyterm -h: expected peek option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q -- "--rows=N" || {
  echo "ptyterm -h: expected rows option in output" >&2
  printf '%s\n' "$out" >&2
  exit 1
}
