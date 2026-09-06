# SaltsNet CNet Load Balancer Design

## Context

The current `lb` module exposes CoroNet objects in public headers and drives
frontend, worker, routing, framing, filtering, and bidirectional pumping from
coroutines. Removing CoroNet therefore requires an API and ownership migration,
not an include-only replacement.

## Decision

`SaltsNet::LB` becomes a caller-driven, single-owner CNet component:

- `salts_lb_create()` allocates fixed connection and per-session frame storage.
- One CNet client owns every accepted frontend and worker connection.
- Two CNet listeners own frontend and worker admission.
- `salts_lb_poll()` is the only progress function and invokes callbacks inline.
- `salts_lb_stop()` closes listener admission and drains CNet; destroy is valid
  only after stop completes.
- CNet receive views are borrowed only during callbacks. Forwarding uses
  `cnet_send()`, which copies into bounded command storage before returning.
- Each stream has at most one receive demand and one peer send in flight. The
  source is re-armed only after the destination send completes, providing
  backpressure without an unbounded relay queue.
- Worker registration, pending frontend data, request frames, and group names
  live in fixed slots owned by the LB. No connection state has a second source.

SESSION mode assigns one worker for the lifetime of a frontend connection.
REQUEST mode uses the framing callback, assigns one worker per frame, returns
the worker to the idle set after one response, and then re-arms the frontend.
Routing and filtering remain synchronous callbacks on the progress owner.

## Error and shutdown semantics

Configuration and capacity errors fail before listeners start. Capacity
exhaustion rejects the newly accepted peer. A protocol/frame overflow closes
that frontend. A failed or closed member closes its paired connection; idle
worker failure removes only that worker. Stop is idempotent, closes both
listeners, then drains all connections within the configured timeout.

## Compatibility

The CoroNet-shaped `coro_lb_*` API is removed rather than emulated because its
context, coroutine, and socket ownership contracts cannot be preserved on a
caller-driven CNet owner. Callers migrate to `salts_lb_*` and explicitly call
`salts_lb_poll()`. The wire protocol is unchanged: workers connect to the
backend listener and, when routing is enabled, send their group name first.

## Verification

Loopback tests use native TCP peers against the public LB API and verify:

- construction, ephemeral listener ports, stop, and destroy;
- SESSION forwarding, group routing, and filter rejection;
- REQUEST framing and worker reuse;
- bounded overflow and shutdown behavior.
