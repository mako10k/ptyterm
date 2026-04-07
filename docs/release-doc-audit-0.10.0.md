# Release Documentation Audit: 0.10.0

## Scope

This audit compares the shipped CLI surface with the user-facing documentation in README and built-in help output for:

- ptyterm
- ptytermd
- ptywrap
- biopen
- pbuf

## Documentation Status By Binary

- `ptyterm`: README examples exist. Text help and structured YAML help exist. Current wording is aligned with the implemented daemon/session-management surface, including `--peek`, `--recv-timeout`, and `--recv-until`.
- `ptytermd`: Text help exists. README now includes a daemon startup/configuration example.
- `ptywrap`: Text help exists. README now includes default and daemon-mode examples.
- `biopen`: No built-in `--help` or `--version` interface exists. README now documents the supported positional interface explicitly.
- `pbuf`: Text help exists. README already includes a primary usage example.

## Man-Page Decision

No project man pages are currently shipped for any binary.

Decision for v0.10.0:

- do not add man pages in this release
- treat README plus built-in help output as the canonical release-facing documentation
- keep the absence of man pages explicit rather than implicit

Rationale:

- the repository currently has no maintained project man-page source set
- adding first-pass man pages now would be a larger documentation project than an audit/alignment pass
- the built-in help output is already tested and was the safer source of truth to align before release

## Mismatches Found And Resolved

- README did not explicitly state documentation status per shipped binary.
- README did not explicitly state that man pages are not shipped in v0.10.0.
- README lacked concrete examples for `ptytermd`, `ptywrap`, and `biopen`.

These were resolved in the same audit pass.

## Remaining Gaps

- `biopen` still lacks a self-describing `--help` / `--version` interface.
- Project man pages remain deferred.

These are known gaps after the audit, not accidental omissions.
