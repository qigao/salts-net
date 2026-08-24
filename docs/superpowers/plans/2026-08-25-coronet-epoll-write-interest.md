# CoroNet Epoll Write Interest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop idle connected TCP streams from keeping the Linux epoll reactor runnable through a permanently armed `EPOLLOUT`, without changing stream send, receive, connect, or shutdown behavior.

**Architecture:** Keep the current single shared epoll reactor and command ring. Make the reactor thread the sole owner of the kernel interest mask: connected streams keep `EPOLLIN | EPOLLRDHUP`, while `EPOLLOUT` is armed only for a pending nonblocking connect or a write ring blocked by `EAGAIN`, and is removed after connect/write completion.

**Tech Stack:** C11, Linux epoll, CoroNet `turbo_stream`, TurboUtils TinyTest, CMake presets, MQTT 5 end-to-end validation.

**Spec:** `/root/dev/runs/20260824T104051Z-flowie-log-p0/artifacts/docker-update-c3d2e7b/retest-20260824T232201Z/epoll-trace.txt` (EU runtime reproduction evidence).

## Global Constraints

- Preserve public CoroNet and Flowie APIs and protocol behavior.
- Do not modify IOCP, kqueue, io_uring, TLS, WebSocket, UDP, or Pipe semantics.
- Keep interest-mask mutation on the epoll reactor thread; callers communicate through the existing command/wake ring.
- Test real TCP sockets and real `epoll_wait`; do not assert source text or a mock reactor.
- Preserve the user's existing dirty `master` worktree until the isolated branch is verified.

---

### Task 1: Reproduce idle EPOLLOUT spinning

**Files:**
- Modify: `CoroNet/tests/test_stream.c`
- Modify: `CoroNet/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: public `turbo_stream_*` TCP APIs and GNU ld `--wrap=epoll_wait` on Linux.
- Produces: a Linux regression named `should park the epoll reactor for an idle connected stream`.

- [ ] **Step 1: Add a real epoll wait counter**

  On Linux, define `__wrap_epoll_wait()` in `test_stream.c`; atomically increment a test counter and delegate every call to `__real_epoll_wait()`. Add `-Wl,--wrap=epoll_wait` only to `test_stream` on Linux.

- [ ] **Step 2: Add the idle persistent-stream test**

  Establish a real loopback TCP client/server stream pair, wait for connect and accept completion, start receive on the accepted stream, snapshot the wait counter, sleep for a fixed 100 ms idle window, and require no more than eight additional `epoll_wait` calls. Then send a literal payload and verify it is received, proving that parking write interest does not strand later writes.

- [ ] **Step 3: Run the Linux test and verify RED**

  Run: `ctest --preset linux-release-user -R '^test_stream$' --output-on-failure`

  Expected: FAIL only at the new idle-wait bound; current code repeatedly returns `EPOLLOUT` for the connected socket.

### Task 2: Make EPOLLOUT demand-driven

**Files:**
- Modify: `CoroNet/src/turbo_stream_epoll.c`

**Interfaces:**
- Consumes: existing `REACTOR_CMD_ADD`/`REACTOR_CMD_WAKE`, `write_ring`, `connect_event_posted`, and reactor-thread callbacks.
- Produces: internal reactor-owned `registered_events` state and an `epoll_ctl(EPOLL_CTL_MOD)` helper; no public API changes.

- [ ] **Step 1: Track the installed kernel mask**

  Add `registered_events` to `stream_epoll_state_t`. Set it only after successful reactor-thread ADD/MOD, and clear it when the fd is disarmed or closed.

- [ ] **Step 2: Add a reactor-only interest update helper**

  Implement a helper that skips unchanged masks, performs `EPOLL_CTL_MOD` for an armed stream fd, records the new mask, and posts the existing stream error event on failure.

- [ ] **Step 3: Register only required initial events**

  Use `EPOLLIN | EPOLLRDHUP` for accepted/connected streams. Add `EPOLLOUT` only while `connect()` reports `EINPROGRESS`; immediate connects do not arm it.

- [ ] **Step 4: Arm and disarm write readiness around backpressure**

  The reactor wake path first attempts to drain the write ring. If `send()` returns `EAGAIN`/`EWOULDBLOCK`, arm `EPOLLOUT`; after the ring drains, remove it unless connect completion is still pending. Retry `EINTR` without changing interest.

- [ ] **Step 5: Run the focused test and verify GREEN**

  Run: `ctest --preset linux-release-user -R '^test_stream$' --output-on-failure`

  Expected: PASS, including idle wait bound and post-idle send content.

### Task 3: Regression and EU runtime verification

**Files:**
- Verify: `CoroNet/tests/test_stream.c`
- Verify: `CoroNet/src/turbo_stream_epoll.c`
- Verify: Flowie EU Docker image built from the combined local source snapshot.

**Interfaces:**
- Consumes: public Windows/Linux presets, CoroNet shutdown suite, Flowie Docker compose deployment, MQTT 5 client tools.
- Produces: test output, CPU samples, MQTT delivery evidence, and container health/log evidence.

- [ ] **Step 1: Run adjacent local regression**

  Run Windows Release `test_stream`, then Linux Release `test_stream` and `ctest -L shutdown` through their documented presets.

- [ ] **Step 2: Review the isolated diff**

  Confirm only the test, Linux test link option, epoll backend, and this plan changed. Check the write-interest state transitions for connect, immediate send, `EAGAIN`, completion, terminal error, close, and shutdown.

- [ ] **Step 3: Integrate without overwriting existing dirty work**

  Apply only the verified hunks to the original dirty `master` worktree, resolve around its logging changes manually, and rerun the focused tests on the combined tree.

- [ ] **Step 4: Build and update the EU Flowie Docker image**

  Build from the eight explicit local source contexts, replace only `flowie-server`, and leave DB/nginx/backend/certbot plus the native debug server untouched.

- [ ] **Step 5: Verify the original production symptom**

  With `picimpact-dev-backend-1` maintaining its MQTT connection, sample Flowie CPU after settling, trace epoll briefly, rerun MQTT 5 QoS 0/1/2 plus the 96-client content test, and require healthy status, zero restarts, and zero WARN/ERROR/FATAL logs.

