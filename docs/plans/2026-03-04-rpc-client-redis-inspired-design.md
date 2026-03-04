# RpcClient Redis-Inspired Optimization Design (2026-03-04)

## Background

This design references the latest `galay-redis` client optimization commit (`293ba71`, 2026-03-04) and applies similar patterns to `galay-rpc` client-related classes.

Target scope in this round:
- `RpcClient` and client-side awaitable flow
- Client-side stream send/recv path (`RpcStream`)
- Client-side read/write hot path helpers in `RpcConn`

Out of scope in this round:
- True concurrent multiplexing semantics on a single connection
- RPC protocol format changes
- Server-side behavior changes

## Goals

- Improve client hot-path performance by reducing avoidable allocations and copies.
- Improve client robustness with explicit connection state checks and consistent error semantics.
- Keep public `RpcClient` API as stable as possible.
- Preserve current single-request-chain behavior on one connection.

## Architecture

Adopt a layered structure inspired by `galay-redis`:

1. `RpcClient` facade (public API compatibility)
- Keep existing methods (`connect`, `call`, `callWithMode`, stream helpers, `close`).
- Delegate state and common resources through an internal runtime context.

2. `RpcClientRuntime` (new internal core)
- Own and coordinate: `TcpSocket`, `RingBuffer`, reader/writer settings, request/stream counters.
- Track connection lifecycle with explicit state:
  - `Disconnected`
  - `Connecting`
  - `Ready`
  - `Closing`
  - `Closed`
- Provide shared helpers:
  - read/write iovec window preparation
  - ring buffer parse loop utilities
  - unified `IOError -> RpcError` mapping

3. Awaitable state-machine layer (refactor)
- Rework call/stream awaitables to explicit state progression with reset/rebind-friendly structure.
- Reuse a common IO hot-path base where feasible.
- Keep timeout support behavior unchanged.

## Data Flow Design

### Connect flow

- `Disconnected -> Connecting`:
  - Create socket and ring buffer only once per connect attempt.
  - Set non-blocking mode.
- `Connecting -> Ready` on success.
- `Connecting -> Disconnected` on failure, with mapped error.
- Reject duplicate `connect()` while `Connecting/Ready/Closing`.

### Unary call flow

- Fast parse first: attempt parse from existing ring-buffer readable bytes before issuing read.
- `WRITEV` phase:
  - Use iovec cursor advancement without repeated vector erase/rebuild.
- `READV` phase:
  - Use fixed iovec window (up to 2 segments from ring buffer).
  - `produce` then parse loop until complete or need more data.
- Validate response:
  - `request_id` and `call_mode` must match.

### Stream client flow

- Stream send/recv awaitables follow same hot-path principles:
  - avoid unnecessary temporary buffers
  - fixed iovec window for reads
  - robust partial write continuation

## Error Handling

Introduce a single mapping function in client runtime path:
- timeout IO errors -> `REQUEST_TIMEOUT`
- disconnect-related IO errors -> `CONNECTION_CLOSED`
- send/recv/system errors -> `INTERNAL_ERROR` (with preserved message)

Additional semantic guarantees:
- Calling data operations when not `Ready` returns deterministic connection-state error.
- `close()` is idempotent (safe to call repeatedly).
- On close/failure, runtime transitions to a reconnectable state.

## Compatibility

- Public API remains source-compatible for current examples and tests.
- No protocol wire-format changes.
- Single-connection call behavior remains non-multiplexed in this round.

## Testing Strategy

1. Unit/behavior tests
- Not-connected call/read/write/stream creation should fail with clear error.
- Duplicate connect / repeated close should follow defined state semantics.
- Response validation failures (`request_id`, `call_mode`) should be deterministic.

2. Integration regression
- Existing `T3-RpcClientTest` scenarios must pass.
- Reconnect-after-close / close-on-error scenarios must be covered.
- Cross-ring-boundary response parsing should remain correct.

3. Performance regression check
- Re-run RPC client benchmark and verify no regression in throughput and latency.

## Risks and Mitigations

- Risk: state transitions introduce behavioral regressions.
  - Mitigation: add explicit state transition tests and preserve current API usage patterns.

- Risk: hot-path refactor breaks parse edge cases.
  - Mitigation: keep parse logic contract unchanged; add boundary tests.

- Risk: interaction with timeout support.
  - Mitigation: reuse existing timeout integration points and add timeout regression tests.
