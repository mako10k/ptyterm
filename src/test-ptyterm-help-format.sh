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

printf '%s\n' "$out" | grep -q 'long: --recv-format' || {
  echo "ptyterm --help-format=yaml: expected recv-format option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --send-stdin' || {
  echo "ptyterm --help-format=yaml: expected send-stdin option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --send-eol' || {
  echo "ptyterm --help-format=yaml: expected send-eol option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --redact-sent' || {
  echo "ptyterm --help-format=yaml: expected redact-sent option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --snapshot' || {
  echo "ptyterm --help-format=yaml: expected snapshot option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --screen' || {
  echo "ptyterm --help-format=yaml: expected screen option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --wait-state' || {
  echo "ptyterm --help-format=yaml: expected wait-state option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --wait-timeout' || {
  echo "ptyterm --help-format=yaml: expected wait-timeout option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --escape' || {
  echo "ptyterm --help-format=yaml: expected escape option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'long: --unescape' || {
  echo "ptyterm --help-format=yaml: expected unescape option entry" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'description: Send input to one managed session' || {
  echo "ptyterm --help-format=yaml: expected send example description" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'description: Send a provider-supplied secret without exposing the secret in argv' || {
  echo "ptyterm --help-format=yaml: expected secret-provider example description" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'command: "secret-provider ROUTER_PASSWORD | .*--send-stdin --send-eol=cr --redact-sent"' || {
  echo "ptyterm --help-format=yaml: expected secret-provider example command" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'description: Use secdat as an optional stdin secret provider for ptyterm' || {
  echo "ptyterm --help-format=yaml: expected secdat integration example description" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'command: "secdat get ROUTER_PASSWORD --stdout | .*--send-stdin --send-eol=cr --redact-sent"' || {
  echo "ptyterm --help-format=yaml: expected secdat integration example command" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q 'secdat integration is optional: secdat can provide bytes on stdout while ptyterm transports them to a managed PTY session' || {
  echo "ptyterm --help-format=yaml: expected secdat integration note" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q "command: \".*--session=1 --recv\"" || {
  echo "ptyterm --help-format=yaml: expected recv example command" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q "command: \".*--session=1 --snapshot\"" || {
  echo "ptyterm --help-format=yaml: expected snapshot example command" >&2
  printf '%s\n' "$out" >&2
  exit 1
}

printf '%s\n' "$out" | grep -q "command: \".*--session=1 --wait-state=snapshot-changed --wait-timeout=2s\"" || {
  echo "ptyterm --help-format=yaml: expected wait-state example command" >&2
  printf '%s\n' "$out" >&2
  exit 1
}
