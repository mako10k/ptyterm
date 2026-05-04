---
applyTo: "src/**"
description: "Use when editing C sources, headers, Automake files, or shell tests under src/ in this Autotools utility repository."
---

# Source Implementation Guidance

This instruction applies to edits under src/ and separates hard invariants from cleanup targets.

## Hard Invariants

- Keep changes local to the owning utility unless behavior is genuinely shared.
- Preserve the current select(2)-based I/O model. Do not introduce a different event mechanism unless the task explicitly requires that architectural change.
- Keep portability and feature detection in configure.ac rather than adding ad-hoc platform ifdefs in C files.
- When a touched C source includes config.h, keep it behind HAVE_CONFIG_H. If the touched file is a legacy exception, normalize it as part of the same change when practical.
- When behavior changes under src/, update the nearest relevant shell test in src/test-*.sh or state why no test applies.

## Cleanup Targets For Touched Legacy Files

- If you touch a main() function that lacks setlocale(LC_ALL, ""), add it unless there is a concrete reason not to.
- If you touch non-English comments, diagnostics, help text, or test expectations in src/, normalize them to English unless the file already has an intentional non-English convention.
- Existing legacy exceptions in untouched files are not precedent for new code.

## Scope Reminder

- Use untouched legacy files as cleanup candidates, not as proof that a weaker convention is acceptable.
- Prefer the smallest durable correction that improves the touched slice while keeping the repository's existing utility boundaries intact.