# turbo_flow Feature Backlog

This document tracks feature work beyond the current email and HTTP adapters.
The goal is to keep `turbo_flow` a data/message orchestration SDK while adding
adapter modules that make it useful as a real orchestrator.

## Current Baseline

Implemented baseline features:

- `.flow` DSL with declarations before orchestration.
- `source`, `stage`, `flow`, `subgraph`, and `A -> B -> C` orchestration.
- Mermaid-style `%%` comments.
- Executor syntax for `inline`, `threadpool` / `thread_pool`, `coro` /
  `coro_pool`, `socks`, and `custom`.
- CoroNet socket source/sink adapters for TCP, UDP, TLS, WS, and WSS.
- Narrow HTTP/1.1 body source/sink adapter.
- SMTP email sink adapter through `email/include/email`.
- Adapter lifecycle hooks: `start`, `consume`, `stop`, and `shutdown`.
- Compile-time validation for unresolved stages, unregistered adapters,
  unregistered custom executors, invalid executor counts, and unsafe fan-in.

Email and HTTP are intentionally not expanded in this backlog except where a
shared feature, such as codecs or retry policy, applies to all adapters.

## Priority 1: Storage Sources And Sinks

Module:

- [ ] Create `turbo_flow_storage`.

Purpose:

- [ ] Let flows read from and write to local durable boundaries without making
  persistence part of `turbo_flow` core.

Initial adapters:

- [ ] File source: read one file as one message.
- [ ] File sink: write each message payload to a configured file path.
- [ ] Append-log sink: append line-delimited or length-prefixed records.
- [ ] Directory source: scan a directory and publish files as messages.

Later adapters:

- [ ] SQLite source/sink.
- [ ] Key-value source/sink if a repository-local storage primitive exists.
- [ ] Directory watch source, if the platform abstraction is available.

Suggested DSL:

```flow
source file_in adapter "file.read"
stage archive adapter "file.write"

flow main {
  file_in -> archive
}
```

Acceptance criteria:

- [ ] Storage adapters live outside `turbo_flow` core.
- [ ] File paths, overwrite mode, append mode, max payload size, and encoding
  mode are configured through adapter config structs, not hard-coded in DSL.
- [ ] Source adapters reject oversized files before allocation.
- [ ] Sink adapters define failure behavior: fail the publish, append
  atomically, or return a partial-write error.
- [ ] Tests cover missing path, empty file, oversized payload, append mode, and
  binary payload.

Risk boundary:

- [ ] No database engine, retention policy, indexing, or query language in core.

## Priority 2: Parser And Codec Adapters

Module:

- [ ] Create `turbo_flow_codec`.

Purpose:

- [ ] Convert transport payloads into structured message forms and encode
  outgoing payloads without binding the core to a specific format.

Initial codecs:

- [ ] Line-delimited framing.
- [ ] Length-prefixed framing.
- [ ] JSON validation and field extraction.
- [ ] CSV row splitter.

Later codecs:

- [ ] MessagePack.
- [ ] Protobuf, only through a thin adapter if schema ownership is clear.
- [ ] MIME part extraction for email payloads, built above the email module.

Suggested DSL:

```flow
stage lines adapter "codec.lines"
stage parse adapter "codec.json"

flow main {
  socket_in -> lines -> parse -> sink
}
```

Acceptance criteria:

- [ ] Codec adapters preserve original payload ownership rules.
- [ ] Parse failures return explicit errors or publish to a configured reject
  sink.
- [ ] JSON field extraction uses a real parser if one is already available in
  the repository or dependency tree.
- [ ] Tests cover partial frames, invalid data, binary payloads, and multiple
  records per input message.

Risk boundary:

- [ ] Do not introduce a generic dynamic object model into core. Structured
  fields should stay in adapter-owned metadata or explicitly typed message
  extensions.

## Priority 3: Metrics And Logging Sinks

Module:

- [ ] Create `turbo_flow_observe`.

Purpose:

- [ ] Make flow execution observable without spreading logging logic through
  the runtime hot path.

Initial features:

- [ ] Stage latency counters.
- [ ] Message count counters.
- [ ] Stage error counters.
- [ ] Adapter start/stop counters.
- [ ] Optional logging sink for selected message summaries.

Suggested DSL:

```flow
stage metrics adapter "observe.metrics"
stage audit adapter "observe.log"

flow main {
  input -> worker -> metrics
  worker -> audit
}
```

Acceptance criteria:

- [ ] Metrics collection is opt-in.
- [ ] Hot paths avoid INFO-level logs per message.
- [ ] Message summaries have explicit size limits and do not log secrets by
  default.
- [ ] Tests cover counters for success, stage error, adapter error, and
  shutdown.

Risk boundary:

- [ ] Do not turn this into a product dashboard or alerting system.

## Priority 4: Timer And Schedule Sources

Module:

- [ ] Create `turbo_flow_schedule`.

Purpose:

- [ ] Trigger flows from time-based events for polling, heartbeat, and batch
  jobs.

Initial sources:

- [ ] Fixed interval source.
- [ ] One-shot delayed source.
- [ ] Limited repeat source.

Suggested DSL:

```flow
source tick adapter "schedule.interval"

flow main {
  tick -> poll -> sink
}
```

Acceptance criteria:

- [ ] Time configuration is held by adapter config structs.
- [ ] Shutdown stops future ticks and drains already-published messages
  according to runtime drain semantics.
- [ ] Tests cover interval tick, one-shot tick, limited repeat, and stop before
  first tick.

Risk boundary:

- [ ] No cron language in the first pass unless there is already a mature parser
  available in the repository.

## Priority 5: Queue And Broker Boundary Adapters

Module:

- [ ] Create `turbo_flow_queue`.

Purpose:

- [ ] Provide local queue boundaries and simple external queue adapters while
  keeping broker semantics outside the core runtime.

Initial adapters:

- [ ] In-memory bounded queue source/sink.
- [ ] Local durable queue if a repository storage primitive can support it.

Later adapters:

- [ ] Redis stream adapter.
- [ ] NATS or Kafka adapter, only as thin optional integrations.

Suggested DSL:

```flow
source queue_in adapter "queue.local"
stage queue_out adapter "queue.local"

flow main {
  queue_in -> transform -> queue_out
}
```

Acceptance criteria:

- [ ] Queue capacity and backpressure behavior are explicit.
- [ ] Sink behavior defines whether full queues fail, block, or drop.
- [ ] Source behavior defines acknowledgement timing.
- [ ] Tests cover queue full, queue empty, shutdown with pending messages, and
  message ownership transfer.

Risk boundary:

- [ ] Do not implement peer discovery, pub/sub protocol compatibility,
  distributed routing sockets, or broker cluster management in `turbo_flow`.

## Priority 6: Routing, Filtering, Join, And Reorder

Module:

- [ ] Add only the core IR support needed for routing and fan-in safety.
- [ ] Keep runtime implementation in `turbo_flow`.
- [ ] Keep expression support in `turbo_flow_expr` where possible.

Purpose:

- [ ] Make the orchestrator able to choose paths and safely combine branches.

Initial features:

- [ ] Conditional route.
- [ ] Named reject route.
- [ ] Key-based partition hint.
- [ ] Reorder strategy for fan-in after unordered worker/threadpool branches.

Later features:

- [ ] Join by key.
- [ ] Windowed join.
- [ ] Deduplicate by key and sequence.

Suggested DSL:

```flow
stage validate
stage good_sink
stage reject_sink

flow main {
  input -> validate
  validate when ok -> good_sink
  validate when error -> reject_sink
}
```

Acceptance criteria:

- [ ] Grammar changes preserve declaration-before-orchestration.
- [ ] Ambiguous fan-in remains rejected unless a reorder strategy is declared.
- [ ] Routing expressions fail fast when fields or operators are unavailable.
- [ ] Tests cover route match, no match, reject path, fan-in reorder, and
  invalid expression syntax.

Risk boundary:

- [ ] Avoid adding a broad scripting language to the core. Keep route
  expressions limited and typed.

## Priority 7: Retry, Dead-Letter, And Failure Policy

Module:

- [ ] Add only the core policy representation needed for validated flow plans.
- [ ] Keep adapter-specific retry execution in adapter modules.

Purpose:

- [ ] Make failures explicit and recoverable without silent fallback.

Initial features:

- [ ] Stage failure policy: fail, retry fixed count, or route to dead-letter.
- [ ] Adapter sink failure policy: fail or route to dead-letter.
- [ ] Failure metadata: stage name, adapter name, error code, attempt count.

Suggested DSL:

```flow
stage send adapter "socket.tcp" retry 3 deadletter dlq
stage dlq adapter "queue.local"

flow main {
  input -> send
}
```

Acceptance criteria:

- [ ] Retries never duplicate ownership of mutable payloads without cloning or
  retaining.
- [ ] Retry policy has an upper bound.
- [ ] Dead-letter messages preserve original payload plus failure metadata.
- [ ] Tests cover retry success, retry exhaustion, dead-letter routing, and
  shutdown during retry.

Risk boundary:

- [ ] No implicit retry by default. Retry must be explicitly configured.

## Priority 8: Additional CoroNet Transports

Module:

- [ ] Extend `turbo_flow_coronet`.

Purpose:

- [ ] Extend the existing socket adapter pattern where CoroNet already provides
  the transport.

Candidate transports:

- [ ] KCP.
- [ ] Pipe.
- [ ] Unix domain socket, if supported on the target platform abstraction.

Suggested DSL:

```flow
source kcp_in adapter "socket.kcp"
stage pipe_out adapter "socket.pipe" exec socks

flow main {
  kcp_in -> transform -> pipe_out
}
```

Acceptance criteria:

- [ ] Each transport is added through the same source/sink adapter shape as
  TCP, UDP, TLS, WS, and WSS.
- [ ] Tests cover source ingress, sink egress, lifecycle shutdown, and invalid
  configuration.

Risk boundary:

- [ ] Transport-specific protocol behavior must not leak into core graph
  planning.

## Cross-Cutting Work

Feature gates:

- [ ] Optional adapter modules should be buildable independently.
- [ ] Core must not depend on storage, codec, schedule, queue, observe, email,
  HTTP, or CoroNet modules.

DSL and parser:

- [ ] Keep Mermaid-inspired syntax only where it helps graph readability.
- [ ] Keep configuration-heavy details in C config structs, not in long DSL
  inline option lists.
- [ ] Add syntax only when the runtime has a tested behavior behind it.

Testing:

- [ ] Each adapter module needs local unit tests.
- [ ] Core grammar changes need parser tests and compile-validation tests.
- [ ] Runtime policy changes need ownership and shutdown tests.

Documentation:

- [ ] Update `turbo_flow.todo.md` when a backlog item becomes part of the core
  architecture.
- [ ] Keep adapter API docs in the adapter module headers.

## Suggested Implementation Order

- [ ] `turbo_flow_storage`: file source, file sink, append-log sink.
- [ ] `turbo_flow_codec`: line framing, length framing, JSON validation.
- [ ] `turbo_flow_observe`: counters and limited audit logging sink.
- [ ] `turbo_flow_schedule`: interval and one-shot source adapters.
- [ ] `turbo_flow_queue`: bounded in-memory queue adapter.
- [ ] Routing and failure policy grammar: conditional route, reject route,
  dead-letter route.
- [ ] Retry and dead-letter runtime behavior.
- [ ] Additional CoroNet transports: KCP, pipe, platform sockets.
