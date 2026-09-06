# SaltsNet ICE CNet Migration Plan

1. Record the current public surface, socket ownership, STUN/TURN transactions, connectivity-check flow, selected-pair progress, tests, and baseline failure.
2. Add a context-free STUN loopback test target that compiles independently of CoroNet and confirm the old API fails it.
3. Implement the bounded private CNet datagram adapter, including numeric peer conversion, hostname resolution, synchronous polling, callback-copy rules, timeouts, and deterministic destruction.
4. Migrate standalone STUN and verify success, transaction filtering, retries, and timeout behavior.
5. Make the TURN client opaque, migrate it to the adapter, and preserve allocation, authentication, permissions, channels, unsolicited delivery, refresh, and maintenance behavior.
6. Remove coroutine context and sockets from the ICE agent, migrate host/server-reflexive/relay candidates and connectivity checks, and make selected-pair progress explicitly caller-driven.
7. Update tests, examples, public documentation, and CMake to link `Salts::CNet` only.
8. Run formatting, targeted builds/tests, CodeGraph impact analysis, adjacent regressions, and CoroNet-reference scans before reporting completion.
