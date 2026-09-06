# SaltsNet CNet Load Balancer Implementation Plan

1. Add failing caller-driven public API and loopback lifecycle/session tests.
2. Implement fixed-slot CNet listener/client ownership and session forwarding.
3. Add routing/filter tests and preserve their synchronous semantics.
4. Add bounded REQUEST framing and worker-reuse tests.
5. Migrate examples and TProxy pump callers to the new API boundary.
6. Remove CoroNet LB sources/headers/build links.
7. Build examples and run LB plus adjacent TProxy tests.
