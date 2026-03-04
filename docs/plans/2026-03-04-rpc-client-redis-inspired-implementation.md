# RpcClient Redis-Inspired Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Refactor `RpcClient`-related classes to adopt `galay-redis`-style hot-path and state-management optimizations while preserving existing public API behavior.

**Architecture:** Keep `RpcClient` as facade and add explicit internal connection-state control and shared error-mapping helpers. Refactor client send/recv awaitables to use fixed read/write iovec windows and parse-first loops to reduce allocations on critical paths. Keep protocol and single-call chain semantics unchanged (no true multiplexing in this round).

**Tech Stack:** C++23, galay-kernel coroutine awaitables, RingBuffer + readv/writev, CMake tests (`T1/T2/T3` + new client regression test).

---

### Task 1: Add client-state regression test scaffold

**Files:**
- Create: `test/T4-RpcClientStateTest.cpp`
- Modify: `test/CMakeLists.txt`
- Test: `test/T4-RpcClientStateTest.cpp`

**Step 1: Write the failing test**

```cpp
Coroutine testCallBeforeConnect(Runtime& runtime, test::TestResultWriter& writer) {
    RpcClient client;
    auto result = co_await client.call("EchoService", "echo", "ping");
    writer.writeTestCase("Call before connect returns error",
                         !result && result.error().code() == RpcErrorCode::CONNECTION_CLOSED,
                         result ? "unexpected success" : result.error().message());
    co_return;
}
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build -j8 --target T4-RpcClientStateTest && ./build/test/T4-RpcClientStateTest`
Expected: FAIL or crash because current client call path assumes connected socket/ring-buffer.

**Step 3: Write minimal test harness implementation**

```cpp
int main() {
    test::TestResultWriter writer("T4-RpcClientStateTest.result");
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();
    runtime.getNextIOScheduler()->spawn(testCallBeforeConnect(runtime, writer));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    runtime.stop();
    writer.writeSummary();
    return writer.failed() > 0 ? 1 : 0;
}
```

**Step 4: Run test to verify it compiles and executes**

Run: `cmake --build build -j8 --target T4-RpcClientStateTest`
Expected: Build succeeds; runtime assertion still fails until `RpcClient` changes land.

**Step 5: Commit**

```bash
git add test/T4-RpcClientStateTest.cpp test/CMakeLists.txt
git commit -m "test(rpc): add rpc client state regression scaffold"
```

### Task 2: Introduce explicit RpcClient connection state guards

**Files:**
- Modify: `galay-rpc/kernel/RpcClient.h`
- Test: `test/T4-RpcClientStateTest.cpp`

**Step 1: Write the failing test for duplicate connect and idempotent close**

```cpp
Coroutine testCloseIdempotentAndDuplicateConnect(RpcClient& client, test::TestResultWriter& writer) {
    auto first_close = co_await client.close();
    auto second_close = co_await client.close();
    writer.writeTestCase("Close is idempotent", first_close.has_value() && second_close.has_value());
    co_return;
}
```

**Step 2: Run test to verify it fails**

Run: `./build/test/T4-RpcClientStateTest`
Expected: FAIL because close/connect lifecycle is not explicitly state-guarded.

**Step 3: Write minimal implementation**

```cpp
enum class ClientState : uint8_t { Disconnected, Connecting, Ready, Closing, Closed };

bool isConnected() const { return m_state.load(std::memory_order_acquire) == ClientState::Ready; }

CloseAwaitable close() {
    if (!m_socket || m_socket->handle() == GHandle::invalid()) {
        m_state.store(ClientState::Closed, std::memory_order_release);
        return CloseAwaitable(nullptr); // replace with safe completed close awaitable helper
    }
    m_state.store(ClientState::Closing, std::memory_order_release);
    return m_socket->close();
}
```

**Step 4: Run tests to verify pass**

Run: `./build/test/T4-RpcClientStateTest`
Expected: New state tests PASS (or move from crash to deterministic errors).

**Step 5: Commit**

```bash
git add galay-rpc/kernel/RpcClient.h test/T4-RpcClientStateTest.cpp
git commit -m "refactor(rpc): add explicit rpc client connection state guards"
```

### Task 3: Unify IOError -> RpcError mapping in client call path

**Files:**
- Modify: `galay-rpc/kernel/RpcClient.h`
- Modify: `galay-rpc/kernel/RpcConn.h`
- Test: `test/T4-RpcClientStateTest.cpp`

**Step 1: Write failing test for deterministic disconnect error mapping**

```cpp
writer.writeTestCase(
    "Disconnected call maps to CONNECTION_CLOSED",
    !result && result.error().code() == RpcErrorCode::CONNECTION_CLOSED);
```

**Step 2: Run targeted test to verify failure**

Run: `./build/test/T4-RpcClientStateTest`
Expected: FAIL due inconsistent mapping across send/recv paths.

**Step 3: Write minimal implementation**

```cpp
inline RpcError mapClientIoError(const IOError& io_error,
                                 RpcErrorCode fallback = RpcErrorCode::INTERNAL_ERROR) {
    if (IOError::contains(io_error.code(), kTimeout)) {
        return RpcError(RpcErrorCode::REQUEST_TIMEOUT, io_error.message());
    }
    if (IOError::contains(io_error.code(), kDisconnectError)) {
        return RpcError(RpcErrorCode::CONNECTION_CLOSED, io_error.message());
    }
    return RpcError(fallback, io_error.message());
}
```

**Step 4: Run tests to verify pass**

Run: `./build/test/T4-RpcClientStateTest && ./build/test/T1-RpcProtocolTest`
Expected: PASS with deterministic error code semantics.

**Step 5: Commit**

```bash
git add galay-rpc/kernel/RpcClient.h galay-rpc/kernel/RpcConn.h test/T4-RpcClientStateTest.cpp
git commit -m "refactor(rpc): unify client ioerror to rpcerror mapping"
```

### Task 4: Refactor RpcClient readv hot path to fixed iovec windows

**Files:**
- Modify: `galay-rpc/kernel/RpcClient.h`
- Modify: `galay-rpc/kernel/RpcConn.h`
- Test: `test/T3-RpcClientTest.cpp`

**Step 1: Write failing regression assertion (functional parity)**

```cpp
if (!result) {
    writer.writeTestCase("Echo call", false, result.error().message());
    co_return;
}
```

**Step 2: Run integration test baseline**

Run: `./build/test/T2-RpcServerTest 9750` (terminal A), then `./build/test/T3-RpcClientTest 127.0.0.1 9750` (terminal B)
Expected: Baseline PASS before refactor.

**Step 3: Write minimal implementation**

```cpp
std::array<struct iovec, 2> m_read_iovecs{};
size_t m_read_iovec_count = 0;

bool prepareReadIovecs() {
    m_read_iovec_count = m_ring_buffer.getWriteIovecs(m_read_iovecs.data(), m_read_iovecs.size());
    m_iovecs.clear();
    for (size_t i = 0; i < m_read_iovec_count; ++i) {
        if (m_read_iovecs[i].iov_len > 0) m_iovecs.push_back(m_read_iovecs[i]);
    }
    return !m_iovecs.empty();
}
```

**Step 4: Run regression tests**

Run: `./build/test/T1-RpcProtocolTest && ./build/test/T3-RpcClientTest 127.0.0.1 9750`
Expected: PASS with same behavior.

**Step 5: Commit**

```bash
git add galay-rpc/kernel/RpcClient.h galay-rpc/kernel/RpcConn.h
git commit -m "perf(rpc): reduce client readv hot-path allocations"
```

### Task 5: Refactor RpcStream client recv path with same parse-first strategy

**Files:**
- Modify: `galay-rpc/kernel/RpcStream.h`
- Modify: `examples/include/E4-StreamClient.cc`
- Test: `examples/include/E4-StreamClient.cc`

**Step 1: Write failing stream-path regression check (example output/assertion)**

```cpp
if (!recv_result) {
    std::cerr << "stream recv failed: " << recv_result.error().message() << "\n";
}
```

**Step 2: Run stream example to capture baseline**

Run: `cmake --build build -j8 --target E3-StreamServer E4-StreamClient`
Expected: Baseline behavior captured.

**Step 3: Write minimal implementation**

```cpp
auto read_iovecs = rb.getReadIovecs();
if (detail::iovecsReadableBytes(read_iovecs) < RPC_HEADER_SIZE) {
    return false;
}
// parse header first, only materialize body buffer when strictly needed
```

**Step 4: Run stream regression**

Run: start `E3-StreamServer`, then run `E4-StreamClient`
Expected: Stream init/data/end behavior unchanged.

**Step 5: Commit**

```bash
git add galay-rpc/kernel/RpcStream.h examples/include/E4-StreamClient.cc
git commit -m "perf(rpc): align stream client parse path with rpc client hot path"
```

### Task 6: Final verification and docs update

**Files:**
- Modify: `docs/03-使用指南.md`
- Modify: `docs/02-API参考.md`
- Test: `test/T1-RpcProtocolTest.cpp`, `test/T3-RpcClientTest.cpp`, `test/T4-RpcClientStateTest.cpp`

**Step 1: Write doc deltas for new state/error semantics**

```md
- `RpcClient` now performs explicit connection-state checks.
- Calling request APIs before connect returns `CONNECTION_CLOSED`.
- `close()` is idempotent.
```

**Step 2: Run full relevant test set**

Run: `cmake --build build -j8 --target T1-RpcProtocolTest T3-RpcClientTest T4-RpcClientStateTest`
Expected: Build PASS.

**Step 3: Execute tests**

Run:
- `./build/test/T1-RpcProtocolTest`
- `./build/test/T4-RpcClientStateTest`
- `./build/test/T2-RpcServerTest 9750` + `./build/test/T3-RpcClientTest 127.0.0.1 9750`
Expected: All PASS.

**Step 4: Run benchmark smoke check**

Run: `cmake --build build -j8 --target B2-RpcBenchClient`
Expected: Benchmark binary builds; quick run shows no obvious regression/crash.

**Step 5: Commit**

```bash
git add docs/03-使用指南.md docs/02-API参考.md
git commit -m "docs(rpc): document rpc client state and error semantics"
```
