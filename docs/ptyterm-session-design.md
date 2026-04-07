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

### Status output shape

All daemon-backed status-producing operations should share the same output convention.

- Human-readable text by default.
- Stable `key=value` lines when `--status-format=kv` is selected.
- Raw payload bytes, when any exist, go to standard output.
- Status metadata goes to standard error.

This should apply consistently to `send`, `recv`, `list`, and `buffer-info`.

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
  ptyterm --recv --session=ID [--recv-size=BYTES] [--recv-timeout=DURATION] [--recv-lines=N]
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
  recv writes payload bytes to stdout and status metadata to stderr.
  Status format 'kv' prints one key=value pair per line.
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

Like `send`, `recv` should use text by default and stable key-value output when `--status-format=kv` is selected.

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

This makes `recv` scriptable using the same parsing strategy as `send`.

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

`recv` returns both payload bytes and status metadata. In text mode, the payload is written to standard output and status text goes to standard error. In `kv` mode, payload handling needs a deterministic split.

Recommended first-version rule:

- `recv` payload always goes to standard output.
- `--status-format=text` prints status to standard error.
- `--status-format=kv` also prints status to standard error in `key=value` form.

This avoids mixing raw session bytes with parseable status lines on the same stream.

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