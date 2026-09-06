# SaltsNet CNet LSQUIC Adapter Plan

1. Add a bounded caller-driven public LSQUIC adapter contract.
2. Add lifecycle and UDP packet-path tests against the contract.
3. Implement address conversion, CNet datagram ownership, partial-send backpressure, and tick-aware poll.
4. Migrate the optional target and one minimal example to the new API.
5. Remove the CoroNet adapter, tests, examples, and build links.
6. Build and repeat targeted tests with LSQUIC enabled, then run CodeGraph impact and reference scans.
