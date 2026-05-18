# ptyterm Session Management Design

## Goal

Translate the session-management idea into a design that fits the current ptyterm implementation model.

- `ptyterm --detach --session=ID`
- `ptyterm --attach --session=ID`
- `ptyterm --list [--session=ID]`
- `ptyterm --send=DATA --session=ID`
- `ptyterm --recv --session=ID`

Constraints:

- `ID` is an integer starting at 1.
- Sessions are isolated per user.
- Backgrounding is only supported for daemon-managed sessions.
- Control traffic uses a Unix domain socket.

## Current State

The current [src/ptyterm.c](../src/ptyterm.c) creates one PTY per invocation and uses the parent process to relay standard I/O to the PTY master with `select(2)`.

- Once the parent exits, no process keeps the PTY master alive.
- There is no session table, session ID, control API, or output history buffer.
- As a result, `detach`, `attach`, `list`, `send`, and `recv` cannot be implemented cleanly inside the current one-shot process model.

The practical conclusion is that a per-user daemon is required.

## Proposed Architecture

### Components

- `ptytermd`
  - One long-lived daemon per user.
  - Owns the session table, PTY masters, child processes, output buffers, and control socket.
- `ptyterm`
  - Keeps the current standalone behavior.
  - Acts as a control client when daemon-backed operations such as `attach`, `list`, `send`, `recv`, or `detach` are requested.
  - May later route new session creation through the daemon as well.

### Why a daemon is required

After `detach`, some process must keep the PTY master open, watch child exit, buffer output, and allow later reattachment. Extending the current parent process into an ad hoc background manager is possible but would mix control-plane and data-plane responsibilities. A dedicated daemon is a cleaner fit.

## Session Model

Each session holds at least the following state.

- `id`: integer greater than or equal to 1, unique per user.
- `child_pid`: PID of the command running on the PTY slave.
- `master_fd`: PTY master owned by the daemon.
- `state`: `attached`, `detached`, or `exited`.
- `client_fd`: currently attached client connection. The first version should allow at most one.
- `command`: command string for display.
- `exit_status`: saved once the child exits.
- `output_ring`: output history ring buffer.
- `recv_offset`: server-side cursor used by `recv`.
- `send_queue`: FIFO queue of pending send requests.
- `send_stream_offset`: absolute byte offset of the session input stream accepted by the daemon.
- `created_at`, `updated_at`: timestamps for listing and garbage collection.

IDs can reuse the smallest available positive integer. That preserves the requested 1-based numbering without introducing global persistent ID management.

## Socket Layout

Use a per-user control socket.

- Preferred: `$XDG_RUNTIME_DIR/ptyterm/daemon.sock`
- Fallback: `/tmp/ptyterm-$UID/daemon.sock`

Create the directory with mode `0700` and the socket with mode `0600`. Also validate peer credentials with `SO_PEERCRED` and reject requests from a different UID.

## CLI Proposal

The command-line interface should be reorganized around four concepts.

- operation: what `ptyterm` should do
- target: which session the operation applies to
- policy: how streaming or backpressure decisions are handled
- formatting: how status is printed

This keeps standalone PTY execution, daemon-backed session management, and machine-readable scripting under one consistent model.

### CLI design principles

- Use long options as the canonical interface.
- Keep short options only for common or historically established cases.
- Use exactly one operation per invocation.
- Use named options for session identity and limits instead of overloading positional arguments.
- Preserve existing behavior through compatibility aliases where practical.

### Common options

These options should be accepted wherever they make sense.

```sh
--help
--version
--socket=PATH
--status-format=text|kv
```

Recommended defaults:

- `--status-format=text` by default.
- `--socket=PATH` overrides the default per-user daemon socket location for management operations.

### Session targeting

Use an explicit session selector for all single-session daemon operations.

```sh
--session=ID
```

Rules:

- `ID` is a positive integer starting at 1.
- `--session` is required for `attach`, `detach`, `send`, `recv`, and `buffer-info`.
- `--session` is optional for `list`; without it, all sessions are listed.

This is more consistent than mixing option names with positional `<id>` arguments.

### Standalone execution mode

The current standalone behavior should remain the default when no daemon-management operation is specified.

Canonical form:

```sh
ptyterm [run-options] [ENVNAME=ENVVALUE ...] [cmd [arg ...]]
```

Recommended canonical run options:

```sh
--stdin=FILE
--stdout=FILE
--stdout-append=FILE
--cols=N
--lines=N
```

Compatibility aliases:

- `-i` remains an alias for `--stdin`
- `-o` remains an alias for `--stdout`
- `-a` remains an alias for `--stdout-append`
- `-c` remains an alias for `--cols`
- `-l` remains an alias for `--lines`

This keeps the original functionality but makes the naming more symmetric around standard stream directions.

### Daemon-backed operations

Operations should be spelled as verbs, with the target supplied separately via `--session`.

Canonical forms:

```sh
ptyterm --attach --session=ID
ptyterm --detach --session=ID
ptyterm --list [--session=ID]
ptyterm --send=DATA --session=ID [--send-policy=block|truncate]
ptyterm --recv --session=ID [--recv-size=BYTES] [--recv-timeout=DURATION] [--recv-lines=N]
ptyterm --buffer-info --session=ID
```

Compatibility aliases:

- `-A` remains an alias for `--attach`
- `-D` remains an alias for `--detach`
- `-L` remains an alias for `--list`
- `-s` remains an alias for `--send`
- `-r` remains an alias for `--recv`
- `-B` remains an alias for `--buffer-info`

Mutual exclusion rules:

- Exactly one operation may be selected.
- Daemon-backed operations cannot be combined with standalone `cmd [arg ...]` execution.
- Operation-specific options must be rejected if their operation is not selected.

### recv option shape

The original compact form `--recv=<spec>` is expressive but inconsistent with the rest of the option model. To preserve functionality without making the interface harder to read, use split options as the canonical form and keep the compact form as a shorthand alias.

Canonical form:

```sh
ptyterm --recv --session=ID [--recv-size=BYTES] [--recv-timeout=DURATION] [--recv-lines=N]
```

Compatibility shorthand:

```sh
ptyterm --recv=size=4k,timeout=1s,lines=10 --session=ID
```

Rules:

- `--recv` with no value selects the operation.
- `--recv-size`, `--recv-timeout`, and `--recv-lines` override the respective defaults.
- If both split options and compact suboptions are given, reject the invocation to avoid ambiguous precedence.

This preserves the compact syntax for experienced users while making the default form easier to scan and script.

### send option shape

Use a matching structure for send.

Canonical form:

```sh
ptyterm --send=DATA --session=ID [--send-policy=block|truncate]
```

Rules:

- `--send=DATA` selects the send operation and provides the payload.
- `--send-policy` controls how incomplete immediate delivery is handled.
- The default send policy is `block`.

This mirrors the recv model: one verb option selects the operation, additional named options refine the behavior.

### Recv output format shape

Recv rendering should stay separate from `--status-format`.

- `--status-format` remains the rendering control for status-oriented operations such as `list` and `buffer-info`, and for recv status lines when recv uses a text-oriented output format.
- `--recv-format` defines how recv payload is rendered on standard output.
- The default recv format should depend on whether standard output is a TTY: use `escaped` for terminal output and `raw` otherwise.

Recommended recv forms:

```sh
ptyterm --recv --session=ID [--recv-size=BYTES] [--recv-timeout=DURATION] [--recv-lines=N] [--recv-format=escaped|raw|json]
```

Rules for recv output formats:

- When `--recv-format` is omitted, select `escaped` if `isatty(STDOUT_FILENO)` is true and `raw` otherwise.
- Escaped mode should reuse the same escape vocabulary already accepted by `--send=DATA` so control bytes such as `\n`, `\r`, `\t`, `\0`, and `\x1e` round-trip predictably.
- Escaped mode should leave ordinary printable bytes readable, escape control bytes and backslashes, and append one terminating newline after each successful non-empty recv result so the next shell prompt starts on its own line.
- `--recv-format=raw` preserves the exact-byte scripting contract by writing the received bytes unchanged to standard output.
- `--recv-format=json` writes one structured envelope to standard output containing the payload plus the same logical recv metadata that is otherwise summarized on standard error.
- The JSON envelope should carry payload bytes in a binary-safe way rather than assuming UTF-8 text.
- This keeps interactive terminal use readable without making redirected or piped recv calls silently lose byte-exact behavior.

Examples:

```sh
ptyterm --recv --session=7
ptyterm --recv --session=7 --recv-format=raw >recv.bin
ptyterm --recv --session=7 --recv-format=json
```

The structured recv envelope should include the session identity, whether the read was `peek`, the stop reason, the next offset, and the payload bytes.

Send does not need a matching `--send-format` option in the first version.

- `--send=DATA` already accepts escape decoding at the CLI boundary.
- The escaped recv form should reuse that same byte notation so users can copy escaped output back into `--send` when needed.
- If a later release needs bulk structured input such as `--send-file`, it can be designed independently from recv rendering.

### Shared byte-notation helpers

The implementation should treat escape and unescape as one shared byte-notation subsystem rather than as separate `send` and `recv` features.

Recommended split:

- a parser helper that converts textual byte notation into raw bytes
- a formatter helper that converts raw bytes into canonical textual notation
- thin operation-specific wrappers that decide where bytes come from, where output is written, and whether any UI-oriented suffix such as a trailing newline is added

This split matches the current code shape well:

- `--send=DATA` already owns the parser side
- escaped `--recv` already owns the formatter side
- the TTY-sensitive recv default and status-line behavior sit above the byte-notation layer and should remain there

In the current implementation, escaped `recv` also appends its trailing newline inside the same local formatter path once escaped mode has been selected. The refactoring target should split that newline policy back out into a wrapper-level decision before the helpers are exposed as standalone filters.

Recommended helper responsibilities:

- `parse_byte_notation(...)` accepts the canonical escaped text form and writes decoded bytes into a caller-supplied buffer
- `format_byte_notation(...)` accepts raw bytes and writes the canonical escaped text form to a caller-supplied sink or buffer
- wrapper code for `--recv-format=escaped` decides whether to append the terminal-facing newline after a successful non-empty recv
- wrapper code for standalone `--escape` and `--unescape` acts as a pure stdin-to-stdout filter and should not add extra framing by default

The canonical notation should be defined once and reused everywhere that textual byte representation appears.

Recommended contract boundaries:

- the shared parser and formatter should not print status lines, usage text, or recv metadata
- the shared parser and formatter should not decide TTY-sensitive defaults
- the shared parser should report invalid sequences in a reusable way so `--send` and standalone `--unescape` can present consistent diagnostics
- the shared formatter should not append a trailing newline on its own; newline insertion is a caller policy

Compatibility notes:

- `--send` should continue accepting the current escape vocabulary during the transition
- escaped `--recv` should move to the canonical formatter, even if backward-compatibility requires temporary acceptance of more than one input spelling on the parser side
- if caret notation such as `^C` is retained only for compatibility, that should be an explicit parser policy rather than part of the formatter output

This gives the new standalone functionality a narrow implementation target: expose the shared parser and formatter through CLI filter modes without coupling them to daemon-specific recv/send control flow.

### Status output shape

All daemon-backed status-producing operations should share the same output convention.

- Human-readable text by default.
- Stable `key=value` lines when `--status-format=kv` is selected.
- For `--recv-format=escaped|raw`, payload output goes to standard output and recv status metadata goes to standard error.
- For `--recv-format=json`, the stdout contract is defined by the selected payload format and standard error should be reserved for fatal diagnostics.

This should apply consistently to `send`, `recv`, `list`, and `buffer-info`.

### Screen-oriented buffer access must stay separate from recv

The existing `recv` contract is a sequential read from the shared session output stream.

- It advances a server-side cursor unless `--peek` is used.
- It is suitable for scripting, line matching, byte inspection, and incremental consumption.
- It should not be repurposed into a terminal-state or screen-snapshot API.

If the project adds terminal-style buffer acquisition later, that should use a separate transport contract and a separate user-facing operation.

- Do not overload `recv` request semantics.
- Do not change `buffer-info` into a screen-state envelope.
- Do not infer a stable screen model from `recv` formatting flags.

This separation keeps the existing byte-stream contract stable while allowing a much richer UI and state model for screen-oriented access.

### Use-case-driven requirements

The design should be anchored in concrete usage rather than in terminal theory alone.

#### Use case 1: human operator checks a long-running task occasionally

Scenario:

- A human starts a long-running command in a managed session.
- They check the session occasionally to confirm whether work is progressing, stalled, waiting for input, or finished.
- They do not necessarily want to attach interactively every time.

What this user needs:

- a quick way to inspect the current visible terminal state
- a readable representation of the current screen, not just a byte stream
- a clear indication of whether the task is still active, waiting, or has returned to the shell

Why `recv` is not enough:

- `recv` answers “what bytes were emitted since my last read?”
- this use case asks “what does the terminal look like now?”
- a human checking sporadically usually wants the current snapshot, not a replay of all intermediate bytes

Derived requirements:

- The separate UI needs a human-readable snapshot mode.
- The snapshot should default to the active visible screen.
- The response should include lightweight session-state context such as foreground task identity and whether the foreground has returned to the shell.
- The first human-oriented UI can ignore attributes visually, but it should not rule them out in the underlying data model.

#### Use case 2: an LLM monitors a medium-running terminal task

Scenario:

- An LLM starts a task that expects a terminal.
- It needs to wait for meaningful state changes rather than poll blindly.
- Useful wait conditions include: a prompt appearing, a timeout elapsing, the foreground process changing, or control returning to the shell.

What this user needs:

- a machine-readable snapshot of terminal state
- stable wait predicates over terminal state changes
- a way to tell the difference between “no output yet”, “output changed but prompt not ready”, and “foreground command finished”

Why `recv` is not enough:

- prompt detection based only on raw output bytes is fragile
- prompt bytes may be obscured by redraws, cursor movement, or full-screen applications
- the agent often needs state-based waiting, not just byte arrival

Derived requirements:

- The separate transport should support structured snapshot output.
- Waiting should be expressed against state predicates, not just byte-stream timeouts.
- Useful first-class predicates include:
  - visible screen changed
  - foreground process group changed
  - foreground returned to the shell
  - cursor moved to a prompt-like location
  - timeout expired without a matching state transition
- Snapshot should be the canonical model; wait and delta features should be defined against snapshot generations or state versions.

#### Use case 3: an LLM drives terminal-style workflows from a non-terminal context

Scenario:

- An LLM needs to perform workflows such as SSH login, remote shell navigation, or remote command execution.
- The controlling side is not itself a human TTY.
- The model must still reason about terminal prompts, password requests, redraws, alternate screens, and shell recovery.

What this user needs:

- a terminal state model that can be consumed programmatically
- reliable send/wait/control loops that are defined in terms of terminal state
- visibility into prompt state, cursor state, active screen, and possibly cell attributes

Why `recv` is not enough:

- raw bytes are not the same thing as terminal state
- SSH and similar flows frequently involve prompt repainting, control sequences, and stateful screen changes
- non-terminal control needs a stable automation contract, not a best-effort textual scrape

Derived requirements:

- The transport must distinguish stream access from terminal-state access.
- The state model should explicitly represent at least:
  - active screen identity
  - visible grid contents
  - cursor position and visibility
  - foreground task identity
  - a monotonic state version or generation for change tracking
- Attributes and alternate-screen state should be part of the transport design even if the first CLI does not render them richly.
- Long term, automation-oriented waiting and delta retrieval likely belong in the same family as screen-state access, not in `recv`.

### What these use cases imply

Across all three use cases, the design pressure is consistent.

- Human inspection wants a readable snapshot.
- Model monitoring wants machine-readable snapshot plus wait predicates.
- Non-terminal control wants a stateful automation contract over the rendered terminal model.

That leads to the following conclusions.

- `recv` should remain a byte-stream operation.
- Screen-state access should be defined as a separate transport family.
- Snapshot is the right base abstraction for the first design pass.
- Wait conditions and later delta modes should be layered on top of a versioned screen-state model.
- Alternate screen, cursor state, and foreground-task state are not optional edge cases; they are core to the motivating use cases.

### Milestone plan

The project should stage this work so that `recv` compatibility remains intact and each step produces a usable, reviewable outcome.

#### Milestone 0: freeze the existing contract

Goal:

- Make the non-goal explicit: screen-oriented access must not change `recv` or `buffer-info` semantics.

Deliverables:

- documented statement that `recv` remains a sequential byte-stream API
- documented statement that new screen-oriented access will use a separate transport family and separate user-facing operation

Exit criteria:

- reviewers agree that existing `recv` wire behavior is a compatibility constraint, not a migration target

Non-goals:

- no new transport messages
- no new CLI operations

#### Milestone 1: define the canonical screen-state model

Goal:

- Decide what the new transport fundamentally returns before designing any CLI or wait behavior.

Scope:

- visible screen snapshot model
- main / alt / active screen semantics
- cursor position and visibility
- foreground task identity and shell-return state
- per-cell attribute model and versioning expectations
- viewport versus scrollback boundary

Deliverables:

- a documented screen-state schema at the conceptual level
- an explicit answer for whether v1 is snapshot-only
- a documented statement of what is guaranteed in v1 versus deferred

Exit criteria:

- the data model is clear enough that a transport payload could be specified without reopening the use-case discussion

Non-goals:

- no delta protocol yet
- no human-readable UI shape yet

#### Milestone 2: define the structured transport

Goal:

- Specify the machine-readable transport for screen-state retrieval independently of any text UI.

Scope:

- request and response message shapes
- versioning model for state snapshots
- screen selector semantics such as `active`, `main`, and `alt`
- error behavior and compatibility rules

Deliverables:

- transport message definitions
- state generation or version semantics
- explicit statement of what state transitions are observable in v1

Exit criteria:

- the transport can deliver full snapshots for use cases 2 and 3 without depending on text scraping

Non-goals:

- no delta retrieval yet unless it is required for transport correctness
- no terminal rendering CLI yet

#### Milestone 3: define state-based wait semantics

Goal:

- Add waiting and monitoring semantics on top of the structured state model.

Scope:

- timeout behavior
- snapshot-changed detection
- foreground process group change detection
- shell-return detection
- prompt-like readiness predicates if they can be specified robustly

Deliverables:

- a wait model defined in terms of terminal state, not output bytes
- documented first-class wait predicates
- documented failure and timeout behavior

Exit criteria:

- use case 2 can be expressed without overloading `recv` timeouts or byte matching as the primary mechanism

Non-goals:

- no human-oriented text formatting decisions yet
- no fine-grained delta optimization unless needed by the wait model

#### Milestone 4: add the first human-oriented UI

Goal:

- Expose a readable inspection command for humans checking long-running sessions occasionally.

Scope:

- one human-readable snapshot operation over the new transport
- sensible defaults for active screen selection
- concise session-state context such as foreground task and shell-return state

Deliverables:

- a user-facing snapshot command distinct from `recv`
- help text and examples for occasional human inspection
- tests covering at least normal shell output and alternate-screen behavior expectations

Exit criteria:

- use case 1 is satisfiable without attaching interactively and without reading raw `recv` output manually

Non-goals:

- no full-screen diff UI
- no rich attribute rendering requirement in the first human-facing output

#### Milestone 5: add automation-oriented UI and deltas if still needed

Goal:

- Add higher-level automation conveniences only after snapshot and wait semantics are proven.

Possible scope:

- machine-oriented CLI output modes
- delta retrieval over versioned snapshots
- richer prompt and readiness helpers
- attribute-aware or structured cursor inspection helpers

Deliverables:

- only the features that remain justified after Milestones 2 through 4 are exercised

Exit criteria:

- use case 3 can be expressed through stable stateful automation primitives rather than ad hoc parsing

Non-goals:

- do not backfill complexity that the structured snapshot plus wait model already solved adequately

### Milestone ordering rationale

This ordering is intentional.

- Milestone 0 protects compatibility before new work starts.
- Milestone 1 prevents the UI from defining the transport accidentally.
- Milestone 2 gives models and tools a stable machine contract.
- Milestone 3 defines monitoring in state terms rather than byte-stream terms.
- Milestone 4 serves the human inspection use case once the underlying contract is settled.
- Milestone 5 is explicitly optional and should be justified by real usage, not by design completeness.

### Milestone 1 decision: canonical screen-state model

The following decisions fix the Milestone 1 baseline.

#### 1. Canonical object: snapshot, not stream replay

The canonical object is a rendered terminal snapshot.

- It represents terminal state at one observation point.
- It is not a replay log.
- It is not defined in terms of `recv` offsets.

This means every later feature such as wait, diff, or human rendering must be explainable as operating on snapshots or transitions between snapshots.

#### 2. v1 scope: viewport snapshot only

Version 1 should model the visible viewport only.

- The viewport is the currently addressable rows and columns of the selected screen.
- Scrollback is explicitly out of scope for v1.
- Historical replay remains the job of `recv` and the output stream buffer.

This keeps the first state model aligned with all three motivating use cases, which primarily ask “what is visible now?” rather than “what was visible 500 lines ago?”.

#### 3. Screen selection model

The state model must distinguish three screen identities.

- `active`: the screen currently presented to an attached terminal
- `main`: the normal screen buffer
- `alt`: the alternate screen buffer

For v1:

- The canonical snapshot always has a selected screen identity.
- The transport model should be able to address `active`, `main`, and `alt` distinctly.
- The initial human-facing UI may default to `active`, but the data model must not collapse `active` into either `main` or `alt`.

If the alternate screen is inactive, the model may still retain its last known contents. Whether the first UI exposes that state directly is a later question, but the transport design must allow it.

#### 4. Snapshot versioning

Each snapshot must carry a monotonic generation value.

- The generation is scoped to terminal-state snapshots, not to `recv` bytes.
- It advances whenever the modeled screen state changes.
- Future wait and delta operations will refer to this generation rather than to byte offsets.

The design does not yet require how generations are encoded on the wire, only that they exist conceptually and are monotonic within a session.

#### 5. Minimum session-state metadata carried with snapshots

The snapshot model must include enough non-cell state to satisfy the use cases.

Required metadata for v1:

- selected screen identity
- rows and columns of the selected viewport
- cursor row and column
- cursor visibility
- foreground process group identity when available
- foreground task name or representative command when available
- whether control appears to have returned to the session shell
- session state such as attached, detached, or exited
- snapshot generation

This is the minimum needed so that humans can inspect status and agents can wait on state transitions without inferring everything from raw bytes.

#### 6. Cell model

The viewport is modeled as a rectangular grid of cells.

Each cell conceptually contains:

- text content for that display position
- display width semantics needed for wide characters
- attribute state

The first human-readable UI may render cells as plain text, but the underlying model must not assume one byte equals one cell or that attributes are absent.

#### 7. Attribute model

Attributes are part of the canonical state model even though the first user-facing UI may not display them richly.

The baseline attribute set for the model should include:

- foreground color
- background color
- bold
- faint
- italic
- underline
- blink
- inverse
- conceal
- strikeout

Additional metadata such as hyperlinks or more specialized OSC-derived state may be deferred, but the core model should assume that cells can carry structured attributes.

#### 8. Deferred areas for v1

The following remain intentionally out of scope for the canonical v1 model.

- scrollback history
- delta encoding
- event streams
- prompt-specific semantics
- attribute-aware human presentation policy
- full terminal mode fidelity beyond what is needed for screen selection and cursor visibility

Those areas may be layered later, but they should not be required to define the v1 snapshot object.

#### 9. Conceptual schema

At the conceptual level, the canonical object is:

```text
terminal_snapshot {
  session_id
  generation
  session_state
  selected_screen: active|main|alt
  viewport {
    rows
    cols
    cursor {
      row
      col
      visible
    }
    cells[row][col] {
      glyph
      width
      attributes
    }
  }
  foreground {
    pgid
    task_name
    shell_returned
  }
}
```

This schema is intentionally conceptual rather than wire-final. Its purpose is to constrain Milestone 2 so that transport design follows an already-decided state model.

### Milestone 2 decision: structured snapshot transport

The structured transport for screen-oriented access is a separate message family from `recv`.

#### 1. Separate operation family

The new transport must not reuse `RECV` request or response messages.

Instead, it introduces a new snapshot-oriented family with its own semantics.

Conceptual operations for v1:

- `SCREEN_SNAPSHOT_REQUEST`
- `SCREEN_SNAPSHOT_RESPONSE`

This family is defined in terms of terminal snapshots, not byte ranges.

#### 2. Request shape

The conceptual request for v1 is:

```text
screen_snapshot_request {
  session_id
  screen_selector: active|main|alt
}
```

Rules:

- `session_id` is required.
- `screen_selector` is required at the transport level even if a user-facing CLI later defaults it.
- The request does not include byte offsets.
- The request does not include wait conditions.
- The request does not include delta cursors.

This keeps Milestone 2 focused on deterministic snapshot retrieval only.

#### 3. Response shape

The conceptual response for v1 is:

```text
screen_snapshot_response {
  session_id
  generation
  selected_screen
  session_state
  foreground {
    pgid
    task_name
    shell_returned
  }
  viewport {
    rows
    cols
    cursor {
      row
      col
      visible
    }
    cells[]
  }
}
```

Rules:

- The response returns one complete snapshot.
- The response is self-describing enough that a client does not need `buffer-info` or `recv` to interpret it.
- The response uses snapshot `generation` as the state-tracking token for later milestones.
- The response may describe `main`, `alt`, or `active`, but it always says which screen was actually returned.

#### 4. Error behavior

The snapshot transport should use the same broad control-plane error style as existing daemon operations while keeping the meaning snapshot-specific.

The transport should distinguish at least:

- session not found
- screen state unavailable for the requested selector
- session exited and no retained snapshot available
- protocol or payload error

The exact wire encoding can follow the existing control-plane error response style. What matters at this stage is that these errors are defined in terms of snapshot semantics rather than byte-stream semantics.

#### 5. Compatibility rule

Adding snapshot transport must not alter the meaning of any existing operation.

- `recv` remains a sequential output-stream read.
- `buffer-info` remains buffer metadata, not screen metadata.
- Existing clients that do not know about snapshot transport continue to function unchanged.

This means the new transport should be additive, not a reinterpretation of existing messages.

#### 6. Why snapshot transport is enough for Milestone 2

This transport already supports the core non-human use cases.

- An LLM can request a machine-readable terminal snapshot.
- A non-terminal controller can reason about prompts, cursor location, and active screen without text scraping.
- Humans are not yet the target of this layer; their dedicated UI belongs to Milestone 4.

What it does not try to solve yet:

- waiting for future changes
- requesting only differences
- presenting a friendly text rendering

Those features depend on the snapshot contract, but they should not complicate the initial transport definition.

### Milestone 3 decision: state-based wait semantics

Wait behavior is defined in terms of snapshot state transitions, not in terms of raw output bytes.

#### 1. Separate wait operation family

Waiting should use a separate operation family layered on top of snapshot generations.

Conceptual operations for v1:

- `SCREEN_WAIT_REQUEST`
- `SCREEN_WAIT_RESPONSE`

These operations do not replace snapshot retrieval. They describe how a client waits for a later snapshot satisfying a state predicate.

#### 2. Wait baseline

Each wait request is anchored to an observed baseline.

Conceptually:

```text
screen_wait_request {
  session_id
  baseline_generation
  timeout_ms
  predicate
}
```

Rules:

- `baseline_generation` is required.
- `timeout_ms` is required.
- A wait succeeds only if a later snapshot satisfies the requested predicate.
- A timeout returns a structured timeout result rather than reusing `recv` timeout wording.

This keeps waiting stateful and explicit.

#### 3. First-class v1 predicates

The first version should support only predicates that can be defined robustly from the canonical snapshot model.

Required predicates for v1:

- `snapshot_changed`
  - succeeds when any later snapshot generation exists
- `foreground_changed`
  - succeeds when the foreground process group identity differs from the baseline snapshot
- `shell_returned`
  - succeeds when the later snapshot indicates that control has returned to the session shell
- `session_exited`
  - succeeds when the session state becomes exited

Optional but still state-based predicate:

- `cursor_changed`
  - succeeds when cursor position or visibility differs from baseline

These predicates directly support use case 2 and most of use case 3 without introducing prompt heuristics prematurely.

#### 4. v1 non-goal: prompt-specific matching

Prompt-specific matching should not be part of the initial wait contract.

Reasons:

- prompts are shell-specific and highly configurable
- many prompts are expressed through redraws rather than stable appended text
- prompt matching often wants policy and user configuration, not transport semantics

Instead, prompt-specific helpers can be layered later on top of snapshot retrieval and baseline wait primitives.

#### 5. Wait result shape

The wait response should always identify both the outcome and the snapshot that resolved it.

Conceptually:

```text
screen_wait_response {
  session_id
  outcome: matched|timeout|session_exited|error
  matched_predicate
  snapshot
}
```

Rules:

- A successful wait returns the resolving snapshot.
- A timeout may return the latest known snapshot if that is useful, but timeout must remain distinguishable from predicate match.
- `session_exited` is a distinct outcome even if no predicate matched first.

This avoids forcing clients to perform an extra fetch just to learn what state ended the wait.

#### 6. Why this is enough for v1

This wait model already supports the core automation loop.

- fetch a baseline snapshot
- issue input if needed
- wait on `snapshot_changed`, `foreground_changed`, `shell_returned`, or `session_exited`
- inspect the returned snapshot and decide the next action

It is intentionally narrower than a generalized event subscription system. The goal is to support reliable terminal workflows first, not to provide a fully generic reactive protocol.

### Screen-oriented design questions

Before implementing a separate screen transport or UI, the project needs explicit answers to the following questions.

#### 1. Primary access model

Should the new operation be line-oriented, screen-oriented, or state-oriented?

- Line-oriented: optimized for users who want visible text rows and do not care about hidden cells, wrapped state, or styling.
- Screen-oriented: returns the current rendered grid, with rows and columns as the primary unit.
- State-oriented: returns a richer terminal model including screen cells, cursor, attributes, scrollback, and alternate-screen state.

Recommendation: treat screen-oriented access as the minimum viable separate UI. A line-oriented view can be derived from it, while a pure line-oriented API would lose too much information too early.

#### 2. Main screen versus alternate screen

Many full-screen programs use the terminal alternate screen buffer.

Open questions:

- Should the UI expose only the currently active screen?
- Should callers be able to request `main`, `alt`, or `active` explicitly?
- If the alternate screen is inactive, should its last contents be retained, cleared, or treated as unavailable?

Recommendation: model main and alternate screens explicitly in the transport. The first UI can default to `active` while still leaving room for `--screen=main|alt|active` later.

#### 3. Snapshot versus delta

There are at least two useful access patterns.

- Snapshot: return the full current screen state.
- Delta: return only cells, cursor movement, or semantic changes since a prior token or generation.

Open questions:

- Is the first version snapshot-only?
- What stability guarantee would a delta cursor or generation ID have?
- Should deltas be cell-based, row-based, or event-based?

Recommendation: start the design with snapshot as the canonical model and define delta later as a derived optimization. Otherwise the initial protocol risks baking in the wrong notion of change.

#### 4. Attribute fidelity

The screen model must decide whether cell attributes are part of the primary contract.

Examples:

- foreground and background color
- bold, faint, italic, underline, blink, inverse, conceal, strikeout
- wide characters and combining characters
- hyperlink and OSC-derived metadata

Open questions:

- Is plain text enough for the first UI?
- If attributes are returned, which subset is guaranteed stable?
- Are attributes exposed only for the cursor cell, or for every visible cell?

Recommendation: if a screen transport is added, define per-cell attributes in the transport even if the first text UI only renders plain text. Omitting them from the wire would make later extension much harder.

#### 5. Scroll and scrollback semantics

Screen access needs an explicit model for visible rows versus scrollback history.

Open questions:

- Is the operation about the visible viewport only, or also scrollback?
- If scrollback is included, is it addressed by line count, absolute position, or a generation token?
- How should apps that repaint the screen continuously interact with scrollback retention?

Recommendation: separate viewport snapshot from scrollback history in both transport and UI. A single operation that tries to cover both usually ends up ambiguous.

#### 6. Cursor and mode state

The cursor is part of terminal state, not just a presentation detail.

Open questions:

- Should the response include cursor row, column, visibility, and shape?
- Are insert mode, origin mode, wrap mode, and keypad mode part of the contract?
- Does the user-facing UI need them all, or only the transport?

Recommendation: include cursor position and visibility in the base model. Other terminal modes can be versioned in later once there is a concrete consumer.

#### 7. User-facing UI shape

The user-facing command should be chosen after the data model is stable.

Candidate directions:

- a text-oriented snapshot command for human inspection
- a structured snapshot command for tooling
- a later delta-oriented command once change semantics are well defined

The important point is not the final option name. The important point is that this UI must be distinct from `recv` and documented as operating on rendered terminal state rather than the raw session output stream.

### Recommended sequencing

To avoid mixing contracts, the work should proceed in this order.

1. Preserve the current `recv` and `buffer-info` wire contracts unchanged.
2. Define the screen-state data model independently from `recv`.
3. Decide whether the first consumer is a human-readable snapshot UI, a structured machine-readable UI, or both.
4. Add the new transport and UI only after the above questions are answered explicitly.

### Future daemon-backed creation mode

If daemon-backed session creation is added later, it should follow the same operation-first pattern rather than overloading the standalone default.

Recommended shape:

```sh
ptyterm --create [run-options] [session-defaults] [ENVNAME=ENVVALUE ...] [cmd [arg ...]]
```

Possible session-default options:

```sh
--output-buffer=SIZE
--overflow=drop|pause
```

This avoids ambiguity between standalone execution and daemon-managed session creation.

### daemon

New command:

```sh
ptytermd [--socket=PATH] [--output-buffer=SIZE] [--overflow=drop|pause]
```

- Creates the per-user control socket.
- Fails if another daemon instance is already active.

Daemon defaults:

- `--output-buffer=SIZE` sets the per-session output history capacity.
- `--overflow=drop` discards old output when the buffer is full.
- `--overflow=pause` stops reading from the PTY while the output history buffer is full.

### Draft help output

The help text should make the operation model obvious on first read.

Goals:

- show standalone execution first, because it remains the default behavior
- show daemon-backed operations as explicit verbs
- keep the canonical long options visible
- mention short-option aliases without making them the primary interface

#### Draft `ptyterm --help`

```text
ptyterm 0.10.0

Usage:
  ptyterm [run-options] [ENVNAME=ENVVALUE ...] [cmd [arg ...]]
  ptyterm --attach --session=ID
  ptyterm --detach --session=ID
  ptyterm --list [--session=ID]
  ptyterm --send=DATA --session=ID [--send-policy=block|truncate]
  ptyterm --recv --session=ID [--recv-size=BYTES] [--recv-timeout=DURATION] [--recv-lines=N] [--recv-format=escaped|raw|json]
  ptyterm --buffer-info --session=ID

Default mode:
  Without a daemon-management operation, ptyterm runs CMD on a PTY and relays
  stdin/stdout/stderr through the PTY.

Common options:
  --help                     Print this help and exit
  --version                  Print version and exit
  --socket=PATH              Override daemon control socket path
  --status-format=text|kv    Status output format for management operations

Run options:
  --stdin=FILE               Read from FILE instead of stdin
  --stdout=FILE              Write to FILE instead of stdout
  --stdout-append=FILE       Append output to FILE instead of stdout
  --cols=N                   Set initial PTY columns
  --lines=N                  Set initial PTY lines

Management operations:
  --attach                   Attach to a daemon-managed session
  --detach                   Detach the currently attached client from a session
  --list                     List sessions, or one session with --session
  --send=DATA                Send decoded bytes to a session input stream
  --recv                     Read from the shared session output stream
  --buffer-info              Show buffer and backpressure state for a session

Management operation options:
  --session=ID               Select target session ID
  --send-policy=block|truncate
                             Send completion policy
  --recv-size=BYTES          Maximum bytes to return from recv
  --recv-timeout=DURATION    Maximum recv wait time
  --recv-lines=N             Maximum lines to return from recv
  --recv-format=escaped|raw|json
                             Recv payload contract

Compatibility aliases:
  -A --attach
  -D --detach
  -L --list
  -s --send
  -r --recv
  -B --buffer-info
  -i --stdin
  -o --stdout
  -a --stdout-append
  -c --cols
  -l --lines

Notes:
  Exactly one management operation may be selected per invocation.
  Management operations cannot be combined with CMD execution.
  recv defaults to escaped stdout rendering when stdout is a TTY, and raw bytes otherwise.
  Escaped recv appends a terminating newline so the next shell prompt is distinct.
  Structured recv output carries both payload and recv metadata on stdout.
  Status format 'kv' prints one key=value pair per line for escaped or raw recv status output.
```

#### Draft `ptytermd --help`

```text
ptytermd 0.10.0

Usage:
  ptytermd [options]

Options:
  --help                     Print this help and exit
  --version                  Print version and exit
  --socket=PATH              Control socket path
  --output-buffer=SIZE       Per-session output buffer size
  --overflow=drop|pause      Output buffer overflow policy

Notes:
  The daemon is per-user.
  The control socket should be created with restricted permissions.
  --overflow=drop keeps the newest output and discards the oldest.
  --overflow=pause stops PTY reads when the user-space output buffer is full.
```

#### Help behavior rules

- `--help` should exit successfully without requiring `--session` or any other operation-specific option.
- If both a management operation and standalone run options are present, the error message should point the user toward the relevant usage line.
- If `--recv` is given with a value, help text should describe it as compatibility shorthand rather than the primary form.
- Short aliases may appear in the help output, but the usage synopsis should prefer canonical long options.

## Control Protocol

### Transport

Use a request/response protocol over a Unix domain socket. `SOCK_STREAM` is sufficient for the first version.

Why:

- `attach` needs a long-lived bidirectional stream.
- `send` and `recv` can share the same transport model.
- `SOCK_SEQPACKET` is also viable, but `SOCK_STREAM` is simpler and broadly portable.

### Framing

Use a fixed-size header followed by an optional payload.

```c
struct ptyterm_msg {
  uint32_t type;
  uint32_t flags;
  uint32_t session_id;
  uint32_t payload_len;
};
```

Suggested message types:

- `CREATE`
- `ATTACH`
- `BUFFER_INFO`
- `DETACH`
- `LIST`
- `SEND`
- `RECV`
- `RESIZE`
- `STATUS`
- `ERROR`
- `DATA`
- `EOF`

During `attach`, `DATA` flows in both directions and the client forwards `SIGWINCH` as `RESIZE`.

## Ring Buffer Behavior

Ring buffer overflow needs an explicit policy because `recv` depends on history retention and `send` behavior changes when output backpressure is enabled.

The daemon should support two overflow policies selected at startup or session creation time.

### Policy 1: `drop`

- The ring buffer stores the most recent output only.
- When new data would overflow the buffer, discard the oldest bytes first.
- Advance `recv_offset` if it points into discarded data.
- Record an overflow counter and a sticky `truncated` flag in the session state.

Effects:

- `recv` may return only the tail of the output stream if the consumer is too slow.
- The daemon never blocks PTY reads just to preserve history.
- Attach latency and session liveness take priority over lossless archival.

### Policy 2: `pause`

- The ring buffer is treated as a hard limit.
- Once the ring is full, the daemon stops reading from the PTY master for that session.
- PTY output remains unread until the client drains data through `attach` forwarding or `recv`.
- No output is discarded by the daemon itself while the session remains within kernel and PTY buffering limits.

Effects:

- Output producers may eventually block once kernel-side PTY buffers also fill.
- This policy preserves user-space history but may stall the child process.
- Attach and `recv` become part of the backpressure-release mechanism.

Suggested implementation details:

- Track `ring_start`, `ring_len`, and `total_output_bytes`.
- Interpret `recv_offset` as an absolute stream offset, not an array index.
- If `recv_offset < oldest_available_offset`, clamp it to `oldest_available_offset` and mark the response as truncated.
- Also track `overflow_policy`, `paused_on_full`, and `dropped_bytes` per session.

Policy-specific notes:

- `drop` is the safer default for general interactive use.
- `pause` is appropriate only when preserving every byte in user space matters more than keeping the child process unstalled.
- Growing the buffer without bound is not recommended for either policy.

## Operation Semantics

### attach

- If the session is `detached`, the daemon transitions it to `attached`.
- The client enters raw mode using logic equivalent to the current [src/ptyterm.c](../src/ptyterm.c).
- The daemon forwards PTY output to the client and also appends it to `output_ring`.
- The first version should allow only one attached client.

### detach

- The daemon closes the attached client connection.
- The session continues running and keeps its child process and PTY master.
- If the target session is not currently attached, the first version should return an error rather than silently succeeding.

### list

The minimal response fields should be:

- `id`
- `state`
- `pid`
- `exit_status` or `-`
- `command`
- `created_at`
- `overflow_policy`
- `truncated` or overflow count
- `buffer_capacity`
- `buffer_used`
- `paused_on_full`

Plain text output is enough for the first version.

### buffer-info

`buffer-info` returns the current output-buffer state for one session.

Suggested fields:

- `id`
- `overflow_policy`
- `buffer_capacity`
- `buffer_used`
- `dropped_bytes`
- `truncated`
- `paused_on_full`
- `oldest_available_offset`
- `next_recv_offset`

This operation exists so scripts can detect whether a paused session is backpressured and how much space remains before attempting `send` or waiting on `recv`.

### send

- Write text or control bytes directly to the PTY master of the target session.
- Decode a dedicated CLI-level send encoding before writing bytes to the PTY master.
- Allow use in both attached and detached states.

Send must consider both the session overflow policy and the selected send policy.

The most natural model is to treat session input as one ordered byte stream shared by all clients. In that model, `send` is not a direct "write now or fail" syscall wrapper. It is a request to append bytes to the session input stream.

Recommended send policies:

- `block`: wait until the full payload has been written, the session exits, or the request is interrupted.
- `truncate`: write only the bytes that can be accepted immediately without waiting for additional progress.

Recommended first-version daemon behavior:

- The request includes a send-policy flag such as `SEND_BLOCK` or `SEND_TRUNCATE`.
- The daemon keeps the PTY master in nonblocking mode.
- Under `block`, the daemon keeps the request pending and continues trying to write as the PTY becomes writable.
- Under `truncate`, the daemon writes as many bytes as possible immediately and then responds without waiting for more room.
- The daemon event loop must remain nonblocking as a whole, even when one client uses `block`.

How send policy interacts with overflow policy:

- Under `drop`, the daemon continues draining PTY output, so `block` usually waits only on PTY input-side pressure.
- Under `pause`, `block` may wait indefinitely if the child is blocked on output and no client drains the session.
- Under `pause`, `truncate` may return zero bytes sent when backpressure is already active.

This model keeps delivery semantics explicit while preserving the nonblocking event-loop design.

#### concurrent send semantics

If another client issues `send` while one `send` request is already in progress, the daemon should serialize requests per session in FIFO order.

Recommended rules:

- Bytes from different `send` requests must never be interleaved.
- Each session has exactly one active send request at the head of `send_queue`.
- Additional send requests are enqueued behind it in arrival order.
- The daemon advances to the next queued request only after the current request finishes with `ok`, `would_block`, `session_exited`, `client_disconnected`, or `interrupted`.

Why FIFO is the natural choice:

- The target program sees one input byte stream, not per-client channels.
- Deterministic ordering matters more than fairness tricks in this kind of PTY control tool.
- FIFO gives scripts a predictable model for retries and replay.

How policies apply to queued requests:

- `block`: once the request reaches the head of the queue, it stays active until completion or interruption.
- `truncate`: once the request reaches the head of the queue, it writes only the bytes immediately accepted and then completes.
- A queued `truncate` request does not skip ahead of a blocking request already at the head.

This avoids surprising reorderings.

#### send and recv symmetry

To make `send` and `recv` symmetrical:

- `recv` operates on one shared session output stream with one server-side read cursor.
- `send` operates on one shared session input stream with one server-side append queue.

In other words:

- `recv` is a single-consumer stream view.
- `send` is a single-ordered-producer-stream view fed by multiple clients.

That symmetry leads to the following usage model:

- Any client may append input with `send`.
- The daemon linearizes all input into one byte stream.
- Any client using `recv` observes output from one shared cursor, not a private subscription.

This is intentionally not a per-client chat model. It is a session-stream model.

Consequences:

- A second client can issue `send` during another client's blocking `send`; its request is queued, not rejected.
- A second client using `recv` still shares the same `recv_offset` and therefore consumes output for all clients.
- If private consumers are needed later, `recv` would need explicit cursor tokens, and `send` could optionally gain explicit transaction IDs. The first version does not need either.

Queued-send cancellation should not be part of the first version.

- Allowing cancellation of not-yet-started queue entries would require stable request IDs, queue inspection, and race handling between dequeue and cancel.
- That would add control-plane complexity without improving the core session-stream model.
- For the first version, once a `send` request is accepted into `send_queue`, it runs to completion, partial completion, or connection-driven termination.
- If cancellation becomes necessary later, add explicit request IDs and a `CANCEL_SEND` operation rather than implicit best-effort behavior.

#### send status reporting

`truncate` is only useful if the caller can easily resend the unsent tail. The `SEND` response should therefore include structured progress information.

Suggested response fields:

- `queue_offset`
- `requested_bytes`
- `sent_bytes`
- `unsent_bytes`
- `resume_offset`
- `blocked`
- `reason`

Semantics:

- `queue_offset` is the absolute input-stream offset at which this request begins.
- `requested_bytes` is the original payload length.
- `sent_bytes` is the number of bytes accepted by the PTY.
- `unsent_bytes` equals `requested_bytes - sent_bytes`.
- `resume_offset` equals `sent_bytes`, so the caller can resend from that byte offset in the original payload.
- `blocked` is true when progress stopped because the PTY could not accept more input immediately.
- `reason` is an enum-like status such as `ok`, `would_block`, `session_exited`, or `interrupted`.

Examples:

- If 100 bytes are requested and 100 are written, return `sent_bytes=100`, `unsent_bytes=0`, `resume_offset=100`, `reason=ok`.
- If 100 bytes are requested and only 24 fit, return `sent_bytes=24`, `unsent_bytes=76`, `resume_offset=24`, `blocked=true`, `reason=would_block`.
- If client B queues a request behind client A, B eventually receives its own `queue_offset` and completion status once A finishes.

The CLI should expose this clearly for scripts, either as a stable text format or as a later machine-readable mode.

Recommended CLI output modes:

- Default: human-oriented text.
- Optional: stable key-value status via `--status-format=kv`.

Example text output:

```text
sent 24/100 bytes; 76 unsent; resume-offset=24; blocked=yes; reason=would_block
```

Example key-value output:

```text
queue_offset=4096
requested_bytes=100
sent_bytes=24
unsent_bytes=76
resume_offset=24
blocked=1
reason=would_block
```

The key-value mode should use one `key=value` pair per line so shell scripts can parse it without JSON tooling.

#### send interruption conditions

For `--send-policy=block`, the request stops waiting and returns when one of the following happens:

- all bytes are written
- the session exits
- the requesting client connection is closed
- the requesting client is interrupted by a signal or cancellation mechanism

Recommended `reason` values:

- `ok`: the full payload was accepted
- `would_block`: partial progress only, used by `truncate`
- `session_exited`: the target session exited before completion
- `client_disconnected`: the requesting control connection closed before completion
- `interrupted`: the requesting client was cancelled by signal or explicit local abort

Detach needs a separate rule.

- `detach` changes the session attachment state for the interactive attached client.
- A `SEND` request is a separate control operation and should not be cancelled merely because the session becomes detached while the request is pending.
- If one client issues `detach` while another client has a blocking `send` in progress, the `send` should continue.
- If the same control connection carrying the blocking `send` is closed, return `reason=client_disconnected`.
- If another client queues a later `send`, the active send continues and the later request remains queued.

This is the most natural model because `detach` is a session-state transition, not a cancellation signal for unrelated control requests.

#### send data encoding

`--send=<data>` should be parsed as a byte string, not as a shell command fragment.

Recommended first-version rules:

- Ordinary characters are sent as their byte sequence exactly as received in `argv`.
- Backslash escapes are decoded by `ptyterm` before transmission.
- Caret notation is supported for common terminal control characters.
- Invalid escape sequences are rejected with a clear error.

Supported backslash escapes:

- `\\` for a literal backslash
- `\^` for a literal caret
- `\n`, `\r`, `\t`, `\e`, `\a`, `\b`, `\f`, `\v`
- `\xHH` for one byte in hexadecimal
- `\ooo` for one byte in octal, with 1 to 3 octal digits

Supported caret notation:

- `^@` through `^_` map to ASCII control bytes `0x00` through `0x1f`
- `^?` maps to `0x7f`
- `^C` therefore sends `0x03`

Recommended disambiguation rules:

- A literal caret must be written as `\^`
- A literal backslash must be written as `\\`
- A trailing bare backslash is an error
- A bare caret not followed by `@`, `A` through `Z`, `[`, `\\`, `]`, `^`, `_`, or `?` is an error

Examples:

```sh
ptyterm --send='hello\n' 1
ptyterm --send='^C' 1
ptyterm --send='large payload' --send-policy=truncate 1
ptyterm --send='\\x1b[6n' 1
ptyterm --send='status:\ ^M?\r' 1
ptyterm --send='\^literal-caret\\literal-backslash' 1
```

Notes:

- Shell quoting still applies before `ptyterm` sees the argument. The examples above assume single quotes in a POSIX shell.
- `^C` sends the byte `0x03`; whether that interrupts the target program depends on the target PTY line discipline and terminal mode.
- Because `--send=<data>` is passed through `argv`, it cannot carry an embedded NUL byte reliably. If NUL support is required, add a later extension such as `--send-hex`, `--send-file`, or `--send-stdin`.

#### protocol handling for SEND

The CLI parser should decode `--send=<data>` into raw bytes before sending the request to the daemon.

- The daemon-side `SEND` request payload should be treated as opaque bytes.
- The daemon should not reinterpret backslashes or caret notation.
- The request should include the selected send policy.
- The response should report `queue_offset`, `requested_bytes`, `sent_bytes`, `unsent_bytes`, `resume_offset`, `blocked`, and `reason`.
- The CLI should format the response as human text by default and as stable `key=value` lines when `--status-format=kv` is selected.
- This keeps shell- and CLI-specific parsing in one place and avoids protocol ambiguity.

### recv

Define `recv` as an API that returns unread output history from the session. The first version should keep one server-side cursor per session.

`recv` should expose stream offsets explicitly, just as `send` exposes `queue_offset` for the input stream.

Advantages:

- Simple polling from scripts.
- No cursor-token protocol required.
- Output observation can be reasoned about as a single ordered byte stream.

Limitations:

- Multiple independent `recv` consumers are not supported.
- If multi-consumer use becomes necessary later, extend the protocol with explicit cursor tokens.

Return when any of these conditions is met:

- requested size reached
- requested line count reached
- timeout reached
- child exited

If data was dropped before the current cursor, return a truncated indicator in the response.

Under the `pause` policy, successful `recv` calls may also clear `paused_on_full` and allow PTY reads to resume once enough space is available in the ring.

#### recv stream offsets

The daemon should track output as one monotonically increasing byte stream.

Recommended fields in the `RECV` response:

- `start_offset`
- `end_offset`
- `next_recv_offset`
- `oldest_available_offset`
- `returned_bytes`
- `truncated`
- `reason`

Semantics:

- `start_offset` is the stream offset at which this response begins.
- `end_offset` is the exclusive offset immediately after the last returned byte.
- `next_recv_offset` is the cursor position the daemon will use for the next `recv`.
- `oldest_available_offset` is the earliest offset still retained in the ring buffer.
- `returned_bytes` equals `end_offset - start_offset`.
- `truncated` is true if the previous cursor had to be advanced because data was already dropped.
- `reason` is an enum-like status such as `ok`, `timeout`, `lines_reached`, `size_reached`, `session_exited`, or `truncated_gap`.

Recommended offset rules:

- Normally, `start_offset` equals the session's current `recv_offset` before consuming output.
- If `recv_offset < oldest_available_offset`, clamp `start_offset` to `oldest_available_offset`, set `truncated=1`, and use `reason=truncated_gap` unless another completion condition is more informative.
- After returning data, set `next_recv_offset = end_offset`.

This mirrors the `send` side closely:

- `send.queue_offset` tells the caller where its input was placed in the shared input stream.
- `recv.start_offset` and `recv.end_offset` tell the caller which portion of the shared output stream was consumed.

#### recv status formatting

For `--recv-format=escaped|raw`, `recv` should use text by default and stable key-value output when `--status-format=kv` is selected.

Example text output:

```text
recv 128 bytes; offsets 4096..4224; next-offset=4224; truncated=no; reason=size_reached
```

Example key-value output:

```text
start_offset=4096
end_offset=4224
next_recv_offset=4224
oldest_available_offset=2048
returned_bytes=128
truncated=0
reason=size_reached
```

This makes escaped and raw `recv` scriptable using the same parsing strategy as `send`, while letting non-interactive stdout keep raw bytes by default.

For `--recv-format=json`, the selected payload envelope becomes the complete stdout contract for successful responses.

- Structured recv output should include both payload bytes and recv metadata together.
- Standard error should be reserved for fatal diagnostics.
- `--status-format` should be rejected together with structured recv formats to avoid two competing metadata channels.

## recv spec

Use `getsubopt(3)` to parse a comma-separated option string.

```text
size=<bytes>
timeout=<duration>
lines=<count>
```

Examples:

```sh
ptyterm -r size=16k,timeout=250ms,lines=10 3
ptyterm -r timeout=2s 1
ptyterm -r 4
```

Defaults:

- `size=4k`
- `timeout=1s`
- `lines=0` for unlimited lines

`recv` returns both payload bytes and status metadata. In escaped and raw modes, the payload is written to standard output and status text goes to standard error. In structured mode, both are serialized together on standard output.

Recommended first-version rule:

- If `--recv-format` is omitted, `recv` chooses `escaped` for TTY stdout and `raw` otherwise.
- `--recv-format=escaped` writes escaped payload text to standard output and appends one terminating newline after each successful non-empty recv result.
- `--recv-format=raw` writes payload bytes unchanged to standard output.
- `--status-format=text` prints escaped or raw recv status to standard error.
- `--status-format=kv` prints escaped or raw recv status to standard error in `key=value` form.
- `--recv-format=json` writes a single structured response to standard output and rejects `--status-format`.

This avoids mixing raw session bytes with parseable status lines on the same stream while still allowing a single-stream structured contract for automation.

Parsing rules:

- `size` accepts `k` and `m` suffixes.
- `timeout` accepts `ms` and `s` suffixes.
- `lines` is a positive integer. `0` may be treated as unlimited internally, but should not be the preferred explicit CLI form.

## Session Lifecycle

1. The daemon creates a new session.
2. The daemon opens a PTY and forks the child command.
3. The initial state is `attached` or `detached`.
4. While attached, the daemon relays traffic between the client and the PTY.
5. On detach, only the client is removed.
6. After child exit, the session enters `exited` and keeps history for a limited time or until explicit cleanup.

Deleting exited sessions immediately would reduce the value of both `list` and `recv`, so some retention window is needed.

## Event Loop Design

Keep the existing `select(2)` style and handle all of the following in one event loop.

- accept on the control socket
- PTY master I/O for each session
- attached client I/O
- child-process reaping

`select(2)` remains a reasonable first choice unless the daemon is expected to manage a very large number of sessions.

## Feasibility Assessment

### Feasible parts

- per-user daemon with UDS control
- `attach`, `detach`, `list`, and `send`
- `getsubopt(3)` parsing for `recv`
- output ring buffer with overflow tracking
- reattach with a single attached client limit

### Non-trivial parts

- raw-mode handling and terminal restoration on abnormal client exit
- `SIGWINCH` forwarding
- history consistency when `recv` and `attach` happen concurrently
- retention and cleanup policy for exited sessions

### Not recommended for the first version

- multiple simultaneously attached clients
- cross-user sharing
- persistent session storage
- full transparent unification of standalone and daemon-backed modes

## Implementation Plan

### Phase 1: daemon skeleton

- Add `src/ptytermd.c`.
- Implement UDS setup, single-instance protection, and `LIST` only.
- Update [src/Makefile.am](../src/Makefile.am) with the new binary and tests.

### Phase 2: create and attach

- Implement PTY and child management inside the daemon.
- Add `ptyterm --attach`.
- Reuse or factor out raw-mode and `SIGWINCH` logic from [src/ptyterm.c](../src/ptyterm.c).

### Phase 3: detach, send, and recv

- Keep the session alive after client disconnect.
- Add escaped control-byte handling for `send`.
- Add `getsubopt(3)`-based `recv`.
- Introduce the output ring buffer and truncation reporting.

### Phase 4: tests

- Add help tests.
- Add shell tests for daemon startup, list, attach/detach, and send/recv.
- Keep timeout-based tests conservative to avoid flaky timing behavior.

## Impact on Existing Code

The existing [src/ptyterm.c](../src/ptyterm.c) would gain two modes: standalone session execution and daemon client mode. To keep the diff manageable:

- factor PTY creation and child launch into helpers
- factor raw terminal handling into helpers
- keep standalone mode as the default
- parse daemon-management options early and branch before the current execution path

This repository generally prefers single-file utilities, but this is one of the few cases where a small shared helper is justified because the behavior is genuinely shared.

## Open Questions

- Should new session creation live in `ptytermd --create ...`, or should `ptyterm` request creation through the daemon?
- Is one `recv` cursor per session enough?
- How long should exited sessions be retained?
- Should `list` stay human-readable only, or should TSV or JSON be considered for automation later?

## Recommendation

This feature set is feasible, but it should be treated as a daemon-backed session manager rather than a small option extension to the current `ptyterm`.

The most realistic first scope is:

1. add `ptytermd`
2. implement `list`, `attach`, `detach`, and `send` first
3. implement `recv` with a single cursor model
4. defer multiple attachments and persistent storage

That scope fits the current codebase without forcing a completely different implementation style.