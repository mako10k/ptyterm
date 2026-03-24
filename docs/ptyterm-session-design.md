# ptyterm Session Management Design

## Goal

Translate the session-management idea into a design that fits the current ptyterm implementation model.

- `ptyterm -D|--detach <id>`
- `ptyterm -A|--attach <id>`
- `ptyterm -L|--list [<id>]`
- `ptyterm -s|--send=<data> <id>`
- `ptyterm -r|--recv=<spec> <id>`

Constraints:

- `<id>` is an integer starting at 1.
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
- `created_at`, `updated_at`: timestamps for listing and garbage collection.

IDs can reuse the smallest available positive integer. That preserves the requested 1-based numbering without introducing global persistent ID management.

## Socket Layout

Use a per-user control socket.

- Preferred: `$XDG_RUNTIME_DIR/ptyterm/daemon.sock`
- Fallback: `/tmp/ptyterm-$UID/daemon.sock`

Create the directory with mode `0700` and the socket with mode `0600`. Also validate peer credentials with `SO_PEERCRED` and reject requests from a different UID.

## CLI Proposal

### daemon

New command:

```sh
ptytermd [--socket=PATH] [--output-buffer=SIZE]
```

- Creates the per-user control socket.
- Fails if another daemon instance is already active.

### client-side operations

Extend `ptyterm` with management options.

```sh
ptyterm -A|--attach <id>
ptyterm -D|--detach <id>
ptyterm -L|--list [<id>]
ptyterm -s|--send=<data> <id>
ptyterm -r|--recv[=<spec>] <id>
```

Mutual exclusion rules:

- Only one management option may be specified per invocation.
- Management options cannot be combined with the existing `cmd [arg ...]` execution mode.

Notes:

- `-L` should allow an omitted `<id>` and list all sessions by default. If `<id>` is present, it may show a single detailed entry.
- `-D` is only valid for daemon-managed sessions. In standalone mode it should fail with a clear error.

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

Ring buffer overflow needs an explicit policy because `recv` depends on history retention.

Recommended first-version behavior:

- The ring buffer stores the most recent output only.
- When new data would overflow the buffer, discard the oldest bytes first.
- Advance `recv_offset` if it points into discarded data.
- Record an overflow counter and a sticky `truncated` flag in the session state.

Effects:

- `recv` may return only the tail of the output stream if the consumer is too slow.
- The daemon never blocks PTY reads just to preserve history.
- Attach latency and session liveness take priority over lossless archival.

Suggested implementation details:

- Track `ring_start`, `ring_len`, and `total_output_bytes`.
- Interpret `recv_offset` as an absolute stream offset, not an array index.
- If `recv_offset < oldest_available_offset`, clamp it to `oldest_available_offset` and mark the response as truncated.

This policy is the best fit for the current codebase. Blocking PTY reads on a full history buffer would risk stalling the child process. Growing the buffer without bound is also a poor default for a long-lived daemon.

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
- `truncated` or overflow count

Plain text output is enough for the first version.

### send

- Write text or control bytes directly to the PTY master of the target session.
- Decode a dedicated CLI-level send encoding before writing bytes to the PTY master.
- Allow use in both attached and detached states.

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
- This keeps shell- and CLI-specific parsing in one place and avoids protocol ambiguity.

### recv

Define `recv` as an API that returns unread output history from the session. The first version should keep one server-side cursor per session.

Advantages:

- Simple polling from scripts.
- No cursor-token protocol required.

Limitations:

- Multiple independent `recv` consumers are not supported.
- If multi-consumer use becomes necessary later, extend the protocol with explicit cursor tokens.

Return when any of these conditions is met:

- requested size reached
- requested line count reached
- timeout reached
- child exited

If data was dropped before the current cursor, return a truncated indicator in the response.

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