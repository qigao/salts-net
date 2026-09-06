# SaltsNet Email CNet Migration Plan

1. Record the existing API, protocol flows, TLS transitions, interruption semantics, tests, and baseline build result.
2. Change tests to require context-free constructors and a CoroNet-free email target; confirm the old implementation fails those requirements.
3. Add a bounded private synchronous CNet transport with connect, send, receive, same-connection TLS upgrade, interrupt, close, and deterministic destruction.
4. Migrate SMTP to the adapter and add plaintext plus STARTTLS loopback coverage.
5. Migrate POP3 and IMAP while preserving response parsing, authentication, and message operations; add focused loopback and validation coverage.
6. Update the unified client, examples, README, and CMake dependency/export surface.
7. Run formatting, targeted builds/tests, CodeGraph impact analysis, adjacent regressions, and CoroNet-reference scans before reporting completion.
