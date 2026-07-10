# turbo_flow TODO

## Goal

Design `turbo_flow` as a data/message processing runtime built from:

- `re2c` for fast lexical scanning.
- `lemon` for deterministic grammar parsing.
- MIR for optional hot-path expression and transform JIT.
- `disruptor_t` for core data passing, stage dependencies, broadcast fan-out/fan-in, and worker-pool load balancing.
- `mem_buffer_t`, `tstr_v`, and `tstr_t` for payload ownership, zero-copy views, and owned transformed strings.

The first version should be a runtime pipeline builder, not a broad code generator.

## Product Scope

`turbo_flow` is SDK-first infrastructure. It can borrow ideas from Splunk,
stream processors, and data pipeline systems, but the product goal is an
embeddable C SDK and runtime, not a full standalone analytics platform.

Primary deliverables:

- C SDK for defining flow graphs, stages, callbacks, executors, and message
  ownership.
- `.flow` DSL for declaring rule topology and data strategies.
- Runtime engine for moving messages through broadcast fan-out/fan-in and
  worker-pool segments.
- Optional adapters for CoroNet sockets, parser modules, logging, metrics, and
  application-specific sinks.
- Tests and benchmarks proving parser/filter/message-processing workloads.

Out of scope for the first product:

- Persistent indexed storage.
- Query UI or dashboard UI.
- Distributed cluster management.
- Message broker semantics like ZeroMQ: routing sockets, peer discovery,
  brokerless transport patterns, pub/sub protocol compatibility, or network
  fabric ownership.
- CI runner semantics like GitHub Actions: job checkout, artifact/cache
  management, hosted runners, secrets management, matrix expansion, or shell
  command orchestration.
- User/account/tenant management.
- Alerting product semantics.
- Long-term log retention and search platform behavior.

The SDK should make those products possible above it, but should not contain
those product responsibilities in the core.

## Adapter Boundary

Network and configuration features are adapters around the flow runtime, not the
runtime identity.

Socket communication:

- Allowed as CoroNet source/sink adapters.
- Allowed for ingesting messages into a flow or emitting processed results.
- Not allowed to turn `turbo_flow` into a message broker or transport protocol.
- Peer discovery, wire compatibility, retry policy, routing sockets, and broker
  topology belong in a higher-level product.

YAML:

- Allowed as an optional configuration front-end that compiles to the same flow
  IR as `.flow`.
- Not allowed to become a CI/job runner language.
- Shell execution, workspace checkout, artifact upload, cache restore, secrets,
  and job matrix semantics belong outside `turbo_flow`.

Core invariant:

```text
turbo_flow core = data/message rule graph SDK
adapters         = sockets, YAML, CLI, CoroNet, storage, metrics, product UX
```

Current implementation status:

- `turbo_flow` core is kept independent from CoroNet; CoroNet socket I/O lives
  in the separate `turbo_flow_coronet` adapter module.
- `.flow` accepts `exec socks` as an alias for the CoroNet/socket executor
  class. This means raw socket data source/sink integration, not SOCKS5 proxy
  behavior.
- `.flow` accepts `exec thread_pool` as an alias for `exec threadpool` and
  `exec coro_pool` as an alias for the Utils coroutine scheduler executor.
  Pool capacity is still declared explicitly with `pool N`.
- Adapter metadata supports `source <name> adapter "<adapter>"` and
  `stage <name> adapter "<adapter>" exec socks`.
- Core adapters support independent `start`, `consume`, `stop`, and `shutdown`
  callbacks. Source adapters may provide only lifecycle callbacks; sink/stage
  adapters used by `exec socks` must provide `consume`.
- `turbo_flow_coronet` has a generic socket adapter for TCP, UDP, TLS, WS, and
  WSS transport configuration. Current tests cover TCP/UDP/TLS/WS/WSS source
  ingress, TCP/UDP/TLS/WS/WSS sink egress, TLS/WSS handshake setup, and
  adapter-owned source context loop lifecycle.
- `turbo_flow_coronet` also has a narrow HTTP/1.1 body source/sink adapter.
  HTTP SOURCE publishes request bodies into the graph; HTTP SINK sends message
  payloads as requests and requires a 2xx response. It intentionally does not
  own routing, retries, redirects, authentication, header mapping, or client
  pooling.
- `turbo_flow_email` has an SMTP sink adapter backed by the repository
  `email/include/email` module. It consumes a flow message payload, builds an
  RFC 2822/MIME message through `email_message_t`, and sends it with
  `smtp_send_message()`.
- If a source adapter creates its own `coro_context_t`, it starts and stops a
  background CoroNet loop. If the host passes a context without ownership
  transfer, the host remains responsible for driving it.
- Email remains an adapter-layer responsibility, not a core DSL/runtime
  dependency. POP3/IMAP source adapters can be added above the same boundary.

## Module Ownership

Keep implementation ownership narrow:

```text
utils/turbo_flow_core
    public SDK types, .flow parser, IR, graph validation, data-plane lowering,
    lifecycle state machine, inline executor, disruptor handoff runtime,
    optional executor adapters over utils primitives such as `turbo_threadpool_t`,
    `coro_scheduler_t`, and `turbo_coro_pool_t`

utils/turbo_flow_expr
    optional expression IR, interpreter backend, MIR backend hooks

CoroNet/turbo_flow_coro
    optional `coro_context_t` integration and socket source/sink adapters

parser/* adapters
    schema extraction, parser-specific stage callbacks, benchmark workloads
```

The core SDK must not depend on CoroNet, YAML, storage engines, or product UI.
Adapters can depend on the core; the core cannot depend on adapters.

## Skill-Driven Design Checklist

Use the project skills as design gates, not background reading.

### `skills/c_design_patterns.md`

Apply it to API shape and ownership:

- Use opaque public handles: `turbo_flow_t`, `turbo_flow_executor_t`,
  `turbo_flow_stage_t`.
- Use factory/builder style for complex configuration instead of long parameter
  lists.
- Use strategy pattern for executor selection and data-plane strategy lowering.
- Use adapter pattern for CoroNet, YAML, parser-specific, storage, metrics, and
  plugin-provided integrations.
- Use Result-style boundary errors only where useful; hot-path callbacks keep
  `int` status codes.
- Every opaque object must have a matching destroy/reset path and documented
  ownership.

Design review questions:

- Is this a stable concept or accidental abstraction?
- Does each object have one owner and one cleanup path?
- Are function pointer tables used only at boundaries, not inside tight loops
  without measurement?

### `skills/plugin_system.md`

Apply it only to extension boundaries, not to the core MVP.

Use plugin-system rules when `turbo_flow` allows external stage packs,
executors, parsers, or sinks:

- Stable C ABI with versioned structs and explicit `sizeof` fields.
- Opaque handles across boundaries.
- Capability and permission metadata for network, file, process, or unsafe
  plugin actions.
- No plugin-to-plugin direct calls; communicate through stage registry, service
  registry, or event interfaces owned by the host.
- Clear lifecycle: discover, validate, load, initialize, register, stop,
  unregister, unload.
- Optional process isolation for untrusted or crash-prone stages.

Do not pull plugin concerns into the core until dynamic external loading is a
real requirement. Static registration is enough for the first SDK.

### `skills/performance_optimization.md`

Apply it to hot-path admission and benchmark requirements:

- Do not optimize by guess. Add benchmarks before introducing MIR, reorder
  buffers, batching, custom lock-free structures, or cache-specific tuning.
- Hot path means more than 1000 ops/s or more than 20% of measured runtime.
- Hot path must avoid allocation, string copies, blocking I/O, logging, and
  high-contention locks.
- Compile/start paths may allocate and validate aggressively; publish/consume
  paths should use preallocated rings, retained buffers, views, and object pools.
- MIR JIT is allowed only after interpreter parity tests and throughput/latency
  benchmarks prove the benefit.
- Any optimization must declare expected effect: throughput, P50/P95/P99
  latency, memory, or startup cost.

Required performance artifacts:

- Parser/filter benchmark before MIR backend.
- Linear graph throughput.
- Fan-out/fan-in throughput.
- Worker-pool throughput and latency.
- Cross-ring handoff cost.
- Async executor completion overhead.

### `skills/turbonet_utils.md`

Apply it as the default implementation toolbox:

- Payload ownership: `mem_buffer_t` + `mem_buffer_retain()` /
  `mem_buffer_release()`.
- Borrowed text/bytes: `tstr_v` or `mem_slice_t`.
- Owned transformed text: `tstr_t`, moved with `tstr_move()` and freed with
  `tstr_freep()`.
- Formatting and diagnostics: `fmt.h`, `tstr_format()`, `tstr_append_format()`.
- Errors: `int` status codes on hot paths; `turbo_error_info()` or structured
  result at API boundaries.
- Stage/IR node allocation: `object_pool_t` for fixed-size nodes, arena/pool for
  parse-time temporaries.
- Data plane: `disruptor_t` broadcast, worker-pool, and
  `disruptor_topology_*()` for dependency graphs.
- Execution: `turbo_threadpool_t` for threadpool executor, `turbo_coro.h` for
  coroutine scheduler executor, `turbo_coro_pool.h` for reusable coroutine
  shells, CoroNet only for event-loop/socket adapters.

Implementation rule:

- If a needed primitive exists in `utils`, use it before adding a new local
  helper or vendor dependency.

## Architecture

```text
.flow DSL
    |
    v
re2c lexer + lemon grammar
    |
    v
flow IR / graph plan
    |
    +--> strategy builder: broadcast graph by default, worker-pool where requested
    +--> validation: names, stage types, cycles, schema, ownership
    |
    v
disruptor topology
    |
    +--> broadcast rings for fan-out and dependency graphs
    +--> worker-pool rings for load-balanced stages
    |
    v
stage callbacks + optional MIR compiled expressions
```

## Message Model

Use one envelope type across the runtime so ownership and stage contracts are explicit:

```c
typedef struct turbo_flow_msg_s {
    uint64_t id;
    uint64_t ts_ns;
    uint32_t type;
    uint32_t flags;
    mem_buffer_t *buffer;
    tstr_v payload;
    tstr_t owned_payload;
    void *parsed;
    int status;
} turbo_flow_msg_t;
```

Rules:

- `buffer` owns raw bytes through `mem_buffer_t` retain/release semantics.
- `payload` is a view into `buffer` unless a stage explicitly replaces it.
- Transformed owned text uses `tstr_t`.
- Parser results must have one owner: either the message, the stage context, or an arena tied to message lifetime.
- A failed stage must set `status` and stop or route according to explicit graph policy.
- Broadcast fan-out observers must not concurrently mutate the same message
  envelope.

## Ownership and Lifetime

Payload lifetime must be closed inside the flow runtime.

Rules:

- `payload` is a borrowed view. It is valid only while the message owns or
  retains the `buffer` that backs it, or while it points into `owned_payload`.
- Stage callbacks must not store `payload.data` after returning. A stage that
  needs to keep bytes must retain `buffer`, clone to `tstr_t`, or move ownership
  into a documented stage-owned object.
- `owned_payload` is optional owned text for transforms that replace the
  payload. When `owned_payload` is non-NULL, `payload` may point into it.
- A ring entry must never contain a dangling `tstr_v`. Cross-ring transfer must
  retain the backing `mem_buffer_t` or move the owned payload explicitly.
- `parsed` must have an explicit owner and destructor. The MVP can require
  callback-owned or message-owned parsed data, but must not leave it as an
  untracked raw pointer.
- `owned_payload` and message-owned `parsed` are unique ownership fields. They
  must never be shallow-copied into another live ring entry.

Message transfer operations:

- `turbo_flow_msg_retain_view(dst, src)`: retain shared `buffer` and copy only
  borrowed views and scalar metadata; requires `src` to have no unique
  `owned_payload` or message-owned `parsed`.
- `turbo_flow_msg_clone(dst, src)`: deep-copy unique payload/parsed data when
  the registered destructors support cloning; otherwise fail compile or fail
  handoff.
- `turbo_flow_msg_move(dst, src)`: move unique `owned_payload` and parsed
  ownership to `dst`, then clear the source fields so source cleanup cannot
  double-free.

Default cross-ring handoff must use retain-view for immutable envelopes or move
for unique ownership. A shallow struct copy is forbidden.

Suggested owned parsed hook:

```c
typedef void (*turbo_flow_destroy_fn)(void *ptr, void *ctx);

typedef struct turbo_flow_msg_s {
    /* fields above */
    void *parsed;
    turbo_flow_destroy_fn parsed_destroy;
    void *parsed_destroy_ctx;
} turbo_flow_msg_t;
```

Message cleanup must release, in order:

1. Message-owned parsed data.
2. `owned_payload`.
3. Retained `buffer`.

## DSL MVP

Start with a small `.flow` grammar:

```text
source input
stage parse_json
stage validate
stage enrich worker 4 exec threadpool workers 4
stage persist

input -> parse_json
parse_json -> validate
validate -> enrich
enrich -> persist
```

Required syntax:

- `source <name>`
- `stage <name>`
- `stage <name> worker <count>`
- `stage <name> exec <inline|threadpool|coro|coronet|custom>`
- `stage <name> worker <count> exec <inline|threadpool|coro|coronet|custom> [workers <count>]`
- `<from> -> <to>`
- grouped dependency shorthand, using comma-separated names:

```text
parse -> [validate, enrich] -> persist
```

Do not add conditionals, loops, persistence, retries, or code generation in the MVP.

## Rule Model

The `.flow` DSL describes data-processing rules and how those rules are mapped
onto the runtime. It should not become a general-purpose programming language.
Complex business logic remains in registered C callbacks or compiled expression
backends.

Rule categories:

- Topology rules: describe how stages connect, including chains, fan-out, fan-in,
  and grouped dependencies.
- Data strategy rules: describe how messages are delivered, with broadcast as
  the default and worker-pool as an explicit load-balancing strategy.
- Execution rules: describe where stage callbacks run: inline, threadpool,
  Utils coroutine scheduler, CoroNet context adapter, or a custom executor.
- Predicate rules: decide whether a message should continue on a branch.
- Routing rules: choose one or more downstream paths based on message fields,
  type, status, or parsed data.
- Transform rules: derive or rewrite message payload, parsed representation,
  metadata, or status.
- Sink rules: terminate a branch by writing to storage, network, logs, metrics,
  or a user callback.
- Error rules: decide whether failure stops the graph, routes to an error stage,
  or marks the message status for later fan-in handling.
- Ownership rules: define whether a stage keeps borrowed views, retains
  `mem_buffer_t`, or creates owned `tstr_t` output.

MVP scope:

- Topology rules.
- Data strategy rules: broadcast and worker-pool.
- Execution rules: inline first, then threadpool / `turbo_coro.h` scheduler /
  CoroNet context / custom adapters.
- Callback-backed transform and sink rules.
- Fail-fast error rules.

Later scope:

- Predicate and routing expressions.
- MIR-backed hot predicates and transforms.
- Named error routes.
- Schema-aware field extraction.
- Stateful windows, joins, or aggregation only after the stateless model is
  stable.

Example:

```text
source input
stage parse
stage validate
stage enrich worker 4 exec threadpool workers 4
stage persist
stage metrics
stage ws_sink exec coronet

input -> parse
parse -> [validate, metrics]
validate -> enrich -> persist
```

This means:

- `input -> parse` is a broadcast chain.
- `parse -> [validate, metrics]` is broadcast fan-out.
- `validate -> enrich -> persist` continues the data graph after validation.
- `enrich worker 4` is a data-plane worker-pool stage: each message is enriched
  by one worker, then completion returns to downstream delivery.
- `exec threadpool workers 4` means the enrich callback runs on a
  `turbo_threadpool_t` executor with four executor workers.
- `ws_sink exec coronet` means the stage requires CoroNet context/socket
  integration.
- The actual parse/validate/enrich/persist/metrics behavior is implemented by
  registered callbacks or expression backends.

## Strategy Construction

`re2c` and `lemon` are responsible for turning `.flow` text into data usage
strategies, not for executing callbacks. The parser should produce an IR that
can be validated and lowered into disruptor rings, dependency gates, and stage
executor bindings.

Default behavior:

- Every normal stage participates in a broadcast data graph.
- Fan-out means one published message is observed by all downstream branches.
- Fan-in means a downstream stage is released only after all dependency branches
  have completed the same sequence.
- Worker-pool is an explicit load-balancing strategy: a message in that segment
  is handled by exactly one worker.

The DSL should make the common data strategies easy:

```text
# broadcast chain
input -> parse -> validate -> persist

# broadcast fan-out/fan-in
parse -> [validate, enrich] -> persist

# worker-pool segment
stage enrich worker 4
validate -> enrich -> persist
```

Lowering rules:

- `a -> b -> c` lowers to a broadcast dependency chain.
- `a -> [b, c]` lowers to broadcast fan-out.
- `[b, c] -> d` lowers to fan-in: `d` depends on both `b` and `c`.
- `stage x worker N` lowers to a worker-pool segment for `x`, with completion
  flowing back to the downstream broadcast graph.
- Mixed broadcast and worker-pool usage may require multiple disruptor rings.
- The parser must reject ambiguous graphs instead of inventing implicit
  fallback behavior.

Suggested internal IR:

```c
typedef enum turbo_flow_data_strategy_e {
    TURBO_FLOW_DATA_BROADCAST = 0,
    TURBO_FLOW_DATA_WORKER_POOL
} turbo_flow_data_strategy_t;

typedef struct turbo_flow_stage_plan_s {
    const char *name;
    turbo_flow_data_strategy_t data_strategy;
    uint32_t data_worker_count;
    /* turbo_flow_exec_config_t is defined in Executor Selection Syntax. */
    turbo_flow_exec_config_t exec;
    uint32_t mutability;
} turbo_flow_stage_plan_t;
```

## Stage Mutability

Fan-out branches can run concurrently, so mutable access must be explicit.

```c
typedef enum turbo_flow_stage_mutability_e {
    TURBO_FLOW_STAGE_READONLY = 0,
    TURBO_FLOW_STAGE_MUTATES_PRIVATE,
    TURBO_FLOW_STAGE_MUTATES_IN_PLACE
} turbo_flow_stage_mutability_t;
```

Rules:

- Stages are read-only by default in broadcast fan-out branches.
- `READONLY` stages receive a const message view and may only write to
  stage-local state or sinks.
- `MUTATES_PRIVATE` stages may modify a private cloned/moved envelope. The flow
  compiler must insert a clone, move, or copy-on-write boundary before the
  stage.
- `MUTATES_IN_PLACE` stages require exclusive ownership of that message envelope
  in the current segment. They are not valid in parallel fan-out unless all
  other branches observe a separate retained/ cloned envelope.
- Compile must reject a graph where two parallel branches can mutate the same
  envelope without an explicit private-copy boundary.

Suggested callback split:

```c
typedef int (*turbo_flow_stage_fn)(turbo_flow_msg_t *msg, void *ctx);
typedef int (*turbo_flow_const_stage_fn)(const turbo_flow_msg_t *msg, void *ctx);
```

The MVP can expose only one registration API, but the stage registration metadata
must still record mutability so lowering can protect fan-out branches.

## Data Plane Strategies

Data-plane strategies define how messages move, duplicate, wait, merge, or get
load-balanced. They do not define what the stage callback does.

MVP strategies:

- Broadcast chain: every stage in the chain observes each message in sequence.
- Fan-out: one upstream message is delivered to multiple downstream branches.
- Fan-in dependency: one downstream stage waits until all required upstream
  branches finish the same sequence.
- Worker-pool: a message is processed by exactly one worker in a stage group.
- Completion bridge: worker-pool completion is published back to a downstream
  broadcast graph.
- Fail-fast stop: failed stage marks status and prevents normal downstream
  release unless an explicit error route exists later.

Near-term strategies:

- Keyed partition: route messages with the same key to the same lane so per-key
  order is preserved while different keys run in parallel.
- Round-robin partition: distribute messages evenly when ordering does not
  matter.
- Predicate branch: publish to a branch only when a predicate passes.
- Selective broadcast: broadcast only to selected branches based on message
  type, flags, or status.
- Error branch: route failed messages to an error stage instead of stopping the
  whole graph.
- Drop policy: drop, reject, or mark overflow when downstream backpressure
  exceeds configured limits.

Later strategies:

- Replay: retain a bounded history and replay messages for late consumers or
  tests.
- Windowing: group messages by count or time for aggregate stages.
- Join: combine messages from multiple sources by key or sequence.
- Priority lane: process urgent messages before normal traffic.
- Rate limit: shape input or stage delivery rate.
- Batch delivery: deliver ranges of sequences to a stage callback for better
  throughput.
- Durable checkpoint: persist cursor progress outside the process. This belongs
  to adapters or products above the core unless a minimal SDK hook is required.

Roadmap candidate values:

```c
/* Not a first public API commitment. */
TURBO_FLOW_DATA_KEYED_PARTITION
TURBO_FLOW_DATA_ROUND_ROBIN
TURBO_FLOW_DATA_PREDICATE_BRANCH
TURBO_FLOW_DATA_ERROR_BRANCH
TURBO_FLOW_DATA_REPLAY
TURBO_FLOW_DATA_WINDOW
TURBO_FLOW_DATA_JOIN
```

The MVP should only implement `BROADCAST` and `WORKER_POOL`. Keep the enum small
in code until tests prove each strategy; this list is a roadmap, not a first
API commitment.

## Cross-Ring Handoff

Mixed broadcast and worker-pool strategies require explicit cross-ring handoff.
The handoff is part of the data plane and must be defined before implementation.

Preferred MVP model:

- Ring entries store a compact message envelope, not raw variable-size payloads.
- The envelope carries retained `mem_buffer_t *buffer`, `tstr_v payload`, optional
  `tstr_t owned_payload`, status, type, flags, and parsed ownership metadata.
- Publishing from one ring to another retains the backing `buffer` and clones or
  moves owned payload only when required by the stage contract.
- The source ring releases its stage dependency only after the target ring has
  accepted the handoff or the handoff has failed with a clear error.
- Worker-pool completion publishes one result envelope back to the downstream
  broadcast ring. Completion is the only point where downstream dependencies are
  released.
- Async executor handoff must copy, retain, or move message ownership out of the
  source slot before the source consumer can release that slot.

Rejected MVP alternatives:

- Passing raw pointers across rings without retain/release.
- Letting worker threads publish directly to arbitrary downstream stages.
- Copying full payload bytes for every handoff by default.
- Using fallback queues when a disruptor ring is full.
- Shallow-copying `owned_payload` or message-owned `parsed` into another ring.

Backpressure:

- If the target ring cannot accept a handoff, the runtime must apply a configured
  policy: block, reject, or mark overflow.
- Silent drop is not allowed in the core MVP.
- Handoff failure must preserve ownership invariants before returning an error.

## Ordering Semantics

Ordering must be explicit because executor completion and worker-pool delivery
can reorder work.

MVP ordering:

- Broadcast stages preserve disruptor sequence order per ring.
- Fan-in waits for all dependency branches for the same sequence.
- Worker-pool stages are unordered by default: each message is processed once,
  but completion order may differ from input order.
- Downstream broadcast after worker-pool receives completion order unless a
  reorder strategy is explicitly configured later.
- A branch that lost input sequence order cannot participate in same-sequence
  fan-in with an ordered branch unless a reorder buffer restores the original
  sequence before fan-in.

Not in MVP:

- Full-order worker-pool completion.
- Per-key ordering.
- Cross-ring global ordering.

Later ordering strategies:

- Per-key ordered worker lanes.
- Reorder buffer with bounded capacity.
- Sequence watermark release.
- Timeout/error behavior for missing sequence completion.

Compile-time ordering validation:

- Mark each stage output as ordered, unordered, or restored-order.
- Broadcast chain preserves ordered.
- Worker-pool marks output unordered unless an explicit reorder strategy is
  configured.
- Fan-in requires all inputs to represent the same original sequence identity.
- Compile must reject ordered + unordered fan-in in the MVP.
- Compile may allow worker-pool fan-in later only after inserting a bounded
  reorder buffer with timeout/error behavior.

## Async Consumer Release Gate

Async executors cannot use `disruptor_consumer_run()` as-is if the callback
returns before stage work is complete. The data plane must not advance a
consumer sequence while any async task still depends on that ring slot.

Rules:

- Inline stages may use synchronous consumer processing and release after the
  callback returns.
- Async stages need a dedicated consumer pump or an explicit pending-completion
  gate.
- The source consumer sequence can advance only after one of these is true:
  - the async task completed and no longer references the source slot;
  - the message was safely retained/cloned/moved out of the source slot before
    release;
  - the stage failed before submit and ownership was cleaned locally.
- Completion callbacks must run exactly once and publish completion back to the
  data plane before downstream dependencies are released.
- Blocking inside `process_batch` to wait for async work is allowed only as a
  simple correctness baseline; it is not the target high-throughput design.

Implementation note:

```text
source ring slot
    -> retain/clone/move envelope for async work
    -> submit to executor
    -> release source slot only after safe ownership transfer
    -> executor done callback publishes completion envelope
```

## Runtime API Sketch

```c
typedef struct turbo_flow_s turbo_flow_t;
typedef struct turbo_flow_stage_s turbo_flow_stage_t;
typedef int (*turbo_flow_stage_fn)(turbo_flow_msg_t *msg, void *ctx);
typedef struct turbo_flow_error_s {
    int code;
    size_t line;
    size_t column;
    char message[256];
} turbo_flow_error_t;

turbo_flow_t *turbo_flow_create(void);
void turbo_flow_destroy(turbo_flow_t *flow);

int turbo_flow_register_stage(turbo_flow_t *flow,
                              const char *name,
                              turbo_flow_stage_fn fn,
                              void *ctx);

int turbo_flow_compile(turbo_flow_t *flow,
                       const char *dsl,
                       size_t len,
                       turbo_flow_error_t *error);
int turbo_flow_start(turbo_flow_t *flow);
int turbo_flow_publish(turbo_flow_t *flow, const turbo_flow_msg_t *msg);
int turbo_flow_stop(turbo_flow_t *flow);
int turbo_flow_reset(turbo_flow_t *flow, int keep_registry);
const turbo_flow_error_t *turbo_flow_last_error(const turbo_flow_t *flow);
```

## Lifecycle State Machine

Registration, compile, start, and stop must be ordered. The runtime should reject
invalid calls instead of trying to repair state.

```text
CREATED
  -> REGISTERING
  -> COMPILED
  -> RUNNING
  -> STOPPING
  -> STOPPED
```

Allowed operations:

- `turbo_flow_register_stage()` is valid only before successful compile.
- `turbo_flow_compile()` freezes the stage registry and produces an immutable
  graph plan. Recompile requires `turbo_flow_reset()` or a new flow object.
- `turbo_flow_start()` is valid only after successful compile.
- `turbo_flow_publish()` is valid only while running, except test-only inline
  execution can expose a separate `turbo_flow_run_once()` API later.
- `turbo_flow_stop()` transitions running flows to stopping and drains or
  cancels according to configured shutdown policy.
- `turbo_flow_reset(flow, keep_registry)` is valid only after `STOPPED` or after
  a failed compile that never reached `RUNNING`.
- `keep_registry != 0` keeps registered stage callbacks and clears compiled
  graph/rings; `keep_registry == 0` returns the flow to empty `CREATED` state.
- Destroying a running flow must either fail fast in debug builds or perform a
  documented stop-and-drain path before releasing resources.

Compile-time validation must check:

- All referenced stages are registered.
- No stage is registered twice.
- Graph syntax is unambiguous.
- Broadcast and worker-pool segments can be lowered to concrete rings.
- Stage executor bindings are available.
- Ownership and cross-ring handoff policies are defined for every segment.

## Stage Callback Model

Use callbacks as the primary processing boundary. A stage should be a small,
typed function that receives one message envelope and returns an explicit status.

```c
typedef int (*turbo_flow_stage_fn)(turbo_flow_msg_t *msg, void *ctx);
```

Callback rules:

- The callback owns no message memory unless ownership is explicitly transferred.
- Mutable callbacks may update `msg->payload`, `msg->parsed`, `msg->type`,
  `msg->flags`, and `msg->status` only when stage mutability metadata grants a
  private or exclusive envelope.
- Read-only callbacks receive a const message view and must not mutate shared
  message state.
- The callback must return 0 on success and a TurboNet error code on failure.
- The runtime decides routing after failure; callbacks should not publish directly to downstream stages in the MVP.
- Stage callbacks must not block unless the stage execution strategy allows blocking.

Separate callback kinds can be added later only when they remove ambiguity:

```c
typedef int (*turbo_flow_predicate_fn)(const turbo_flow_msg_t *msg, void *ctx);
typedef int (*turbo_flow_transform_fn)(turbo_flow_msg_t *msg, void *ctx);
typedef int (*turbo_flow_sink_fn)(const turbo_flow_msg_t *msg, void *ctx);
```

Do not expose these as separate public registration APIs until the basic
`turbo_flow_stage_fn` contract is proven.

## Execution Strategies

`turbo_flow` should separate stage behavior from stage execution. The callback
defines what to do; the execution strategy defines where and how it runs.
`disruptor_t` is not an execution strategy in this model. It is the core data
transport and dependency gate used to move messages between stages.

```c
typedef enum turbo_flow_exec_kind_e {
    TURBO_FLOW_EXEC_INLINE = 0,
    TURBO_FLOW_EXEC_THREADPOOL,
    TURBO_FLOW_EXEC_CORO_SCHEDULER,
    TURBO_FLOW_EXEC_CORONET_CONTEXT,
    TURBO_FLOW_EXEC_CUSTOM
} turbo_flow_exec_kind_t;
```

Planned strategy boundaries:

- `TURBO_FLOW_EXEC_INLINE`: tests, deterministic single-thread execution, low-volume local pipelines.
- `TURBO_FLOW_EXEC_THREADPOOL`: CPU-bound or blocking stage work where order is not the primary constraint.
- `TURBO_FLOW_EXEC_CORO_SCHEDULER`: cooperative coroutine stages built on
  Utils `turbo_coro.h` and, for high-frequency jobs, `turbo_coro_pool.h`.
- `TURBO_FLOW_EXEC_CORONET_CONTEXT`: CoroNet adapter for stages that need
  `coro_context_t`, event-loop integration, or coroutine socket I/O.
- `TURBO_FLOW_EXEC_CUSTOM`: user-provided scheduler for embedding into an existing runtime.

The MVP should implement inline first and use `disruptor_t` as the internal data
plane for multi-stage delivery. Threadpool, coroutine, and custom execution
should be adapters, not hard dependencies of the core parser/graph compiler.

### Executor Selection Syntax

Executor selection is orthogonal to data strategy.

```text
stage parse exec inline
stage enrich worker 4 exec threadpool workers 4
stage fetch exec coro lanes 2 pool 128
stage ws_ingest exec coronet
stage custom_sink exec custom "sink_exec"
```

Semantics:

- `worker N` immediately after the stage name is a data-plane strategy. It means
  the stage is lowered to a worker-pool segment and each message is processed by
  exactly one worker in that segment.
- `exec <kind>` chooses where the stage callback runs.
- `exec threadpool workers N` configures executor workers; it does not by
  itself change broadcast vs worker-pool delivery.
- `exec coro lanes N pool M` configures pure Utils coroutine scheduler lanes and
  optional `turbo_coro_pool_t` capacity.
- `exec coronet` requires a CoroNet adapter and is valid only when the host
  provides `coro_context_t` integration.
- `exec custom "<name>"` binds to an executor registered by SDK before compile.

Defaults:

- Data strategy default: broadcast.
- Executor default: inline.
- `stage x worker N` without `exec` means worker-pool data delivery with inline
  callback execution in the worker-pool consumer.
- `stage x exec threadpool` without `workers` uses executor default worker
  count from SDK config.
- `stage x exec coro` without `lanes` uses one coroutine scheduler lane.
- `stage x exec coro` without `pool` can create coroutines directly; hot paths
  should set or inherit a `turbo_coro_pool_t` capacity.

Suggested stage execution config:

```c
typedef enum turbo_flow_exec_kind_e {
    TURBO_FLOW_EXEC_INLINE = 0,
    TURBO_FLOW_EXEC_THREADPOOL,
    TURBO_FLOW_EXEC_CORO_SCHEDULER,
    TURBO_FLOW_EXEC_CORONET_CONTEXT,
    TURBO_FLOW_EXEC_CUSTOM
} turbo_flow_exec_kind_t;

typedef struct turbo_flow_exec_config_s {
    turbo_flow_exec_kind_t kind;
    uint32_t workers;      /* threadpool workers or custom executor hint */
    uint32_t lanes;        /* coroutine scheduler lanes */
    uint32_t pool_capacity;/* turbo_coro_pool_t capacity, 0 = default/direct */
    const char *custom_name;
} turbo_flow_exec_config_t;
```

SDK registration should allow both callback and default executor metadata:

```c
int turbo_flow_register_stage_ex(turbo_flow_t *flow,
                                 const char *name,
                                 turbo_flow_stage_fn fn,
                                 void *ctx,
                                 const turbo_flow_stage_options_t *options);
```

Compile-time validation:

- Reject unknown executor names or executor kinds unavailable in the current
  build.
- Reject `exec coronet` in core-only builds without the CoroNet adapter.
- Reject `exec custom` when `custom_name` was not registered before compile.
- Reject executor worker/lane counts of zero when explicitly specified.
- Reject full-order fan-in from unordered worker-pool or threadpool completion
  unless a reorder strategy is configured.
- Reject a blocking stage on inline or coro executor unless stage metadata marks
  it as allowed and the shutdown policy can interrupt it.
- Reject `exec coro` stages that call CoroNet socket APIs unless they use the
  CoroNet context executor.

## Executor Strategies

Executor strategies define how a stage callback is scheduled and completed.
They do not define fan-out, fan-in, worker-pool delivery, or message ownership;
those belong to the data plane.

MVP executor:

- Inline executor: run the callback immediately on the caller/runtime thread.
  This is deterministic, easy to test, and useful for low-volume embedded use.

Near-term executors:

- Threadpool executor: submit callback work to `turbo_threadpool_t`; good for
  CPU-heavy, blocking, or parallel stage work.
- TurboCoro scheduler executor: run callbacks through Utils `turbo_coro.h`
  primitives, `coro_scheduler_t`, and optional `turbo_coro_pool_t`; good for
  cooperative state machines that yield explicitly.
- CoroNet context executor: adapt coroutine execution to `coro_context_t` when
  event-loop, socket, TLS, WebSocket, UDP, KCP, or pipe integration is required.
- Custom executor: let embedding applications provide their own scheduler,
  reactor, game loop, real-time loop, or service runtime.

Later executors:

- Batch executor: invoke a stage with a sequence range instead of one message at
  a time.
- Affinity executor: pin a stage or key partition to a specific thread, core,
  coroutine scheduler, or external runtime lane.
- Priority executor: prefer urgent stage work while preserving data-plane
  correctness.
- Timed executor: schedule delayed or periodic stage callbacks.
- Isolated executor: run risky or plugin-provided callbacks behind a process or
  sandbox boundary. This belongs above the core unless a product requires it.

Each executor must declare:

- Ordering: unordered, per-key ordered, or fully ordered.
- Blocking policy: callback may block, may yield, or must be nonblocking.
- Completion model: synchronous return, async completion callback, or completion
  ring publish.
- Shutdown behavior: drain, cancel, or stop accepting new work.
- Backpressure behavior: reject submit, block submit, or queue with a bounded
  capacity.
- Ownership behavior: whether the runtime must retain `mem_buffer_t` before
  submit and when it is released.
- Thread-safety: whether `submit`, `drain`, and `shutdown` can be called from
  multiple threads.

Executor interfaces must carry completion explicitly. The inline executor can
complete before returning; async executors must report completion exactly once
after the callback finishes or fails.

## Data Plane vs Execution Plane

Keep these responsibilities separate:

```text
data plane:
    disruptor rings, message ownership, dependency gates, fan-out/fan-in,
    worker-pool delivery semantics, backpressure

execution plane:
    where callbacks run: inline, threadpool, coroutine scheduler,
    CoroNet context, or custom executor
```

`disruptor_t` should remain the central message-passing primitive. Executors
pull from, receive from, or publish back to the data plane. They do not own the
graph topology.

Suggested executor interface:

```c
typedef struct turbo_flow_executor_s turbo_flow_executor_t;

typedef void (*turbo_flow_executor_complete_fn)(void *complete_ctx,
                                                turbo_flow_msg_t *msg,
                                                int status);

typedef int (*turbo_flow_executor_submit_fn)(void *ctx,
                                             turbo_flow_stage_fn fn,
                                             turbo_flow_msg_t *msg,
                                             void *stage_ctx,
                                             turbo_flow_executor_complete_fn done,
                                             void *done_ctx);

typedef struct turbo_flow_executor_ops_s {
    turbo_flow_executor_submit_fn submit;
    void (*drain)(void *ctx);
    void (*shutdown)(void *ctx);
} turbo_flow_executor_ops_t;
```

Rules:

- Inline executor calls the callback immediately.
- Threadpool executor submits a retained message to `turbo_threadpool_t`.
- Coroutine scheduler executor runs work through Utils `turbo_coro.h`
  primitives. It may use `coro_scheduler_t` and `turbo_coro_pool_t` for pure
  cooperative scheduling.
- CoroNet context executor is a separate adapter for stages that must integrate
  with an event loop or coroutine sockets.
- Custom executor must document whether it preserves order, supports blocking,
  and how completion is reported.
- Completion publishes status and modified message state back into the flow data
  plane before downstream dependencies are released.
- Async executors must call the completion callback exactly once.

Coroutine scheduler executor resource model:

- Own one or more `coro_scheduler_t` instances according to configured lanes.
- Use `turbo_coro_pool_t` for high-frequency stage jobs; do not hand-roll a
  coroutine free-list inside `turbo_flow`.
- Keep stage user data separate from lifecycle metadata. Stage callbacks may use
  `coro_get_data()` / `coro_set_data()`; pool/executor metadata belongs behind
  `turbo_coro_pool_t` and the coroutine owner-data hook.
- A coroutine job completes by returning from its entry function; completion
  cleanup must publish status back to the flow completion gate exactly once.
- Shutdown must stop accepting jobs, tick/drain according to policy, then
  destroy schedulers before destroying pools.
- If a scheduler is force-destroyed with live pooled coroutines, the pool
  discard hook must clean active bookkeeping before pool destruction.

## Threadpool Integration

Use `turbo_threadpool_t` for stages that are naturally job-like:

- CPU-heavy parsing or compression.
- Blocking calls that cannot be converted to coroutine I/O.
- Worker-pool stages where exactly one worker should process each message.
- Background MIR compile jobs only before `turbo_flow_start()`; hot-path lazy
  compilation is not allowed in the MVP.

Threadpool rules:

- The flow runtime must retain message buffers before submitting a job.
- The worker must release retained buffers after stage completion.
- Completion must publish the result back into the next ring or completion gate.
- A threadpool stage must define ordering semantics: unordered, keyed-order, or full-order.
- Full-order completion requires a reorder buffer and should not be the default.

Recommended mapping:

```text
disruptor broadcast stage
    |
    v
turbo_threadpool_t jobs
    |
    v
completion ring / downstream disruptor stage
```

## TurboCoro Integration

Use Utils `turbo_coro.h` for coroutine-based execution. This means the executor
is built around stackful cooperative coroutines without requiring CoroNet:

- `coro_create()` / `coro_resume()` / `coro_yield()` for manual low-level control.
- `coro_scheduler_t` / `coro_spawn()` / `coro_scheduler_tick()` for pure cooperative scheduling.
- `turbo_coro_pool_t` / `turbo_coro_spawn_pooled()` when high-frequency coroutine jobs need object reuse.

Use `coro_context_t` only when the stage also needs CoroNet event-loop or socket
integration. `coro_context_t` and `coro_object_pool_*` are adapter layers above
the coroutine primitive and generic pool, not the definition of the coroutine
execution model.

Good coroutine candidates:

- Cooperative stages that frequently yield while waiting for async progress.
- Flow-local state machines that benefit from stackful sequencing.
- Socket source stages through a `coro_context_t` adapter.
- WebSocket / TCP / UDP ingress through CoroNet source/sink adapters.
- Async sink stages that write to network services.
- Request/response enrichment where waiting should not block a worker thread.

Core dependency rule:

- `utils`-level `turbo_flow` must not depend directly on CoroNet headers.
- `utils`-level `turbo_flow` may depend on Utils `turbo_coro.h` and
  `turbo_coro_pool.h` for pure coroutine scheduler execution.
- CoroNet should provide a `turbo_flow_coronet` adapter if `coro_context_t` or
  socket sources/sinks are needed.
- Coroutine stages must publish into flow through a thread-safe source adapter.
- Flow shutdown must interrupt pending coroutine waits before destroying message buffers.

Suggested split:

```text
utils/turbo_flow_core
    parser, graph plan, stage registry, disruptor runtime, inline executor,
    threadpool executor adapter, pure turbo_coro scheduler executor

CoroNet/turbo_flow_coronet
    coro_context_t executor adapter, socket ingress/egress source and sink
    adapters, CoroNet lifecycle integration
```

This keeps the data pipeline reusable by non-network tools while still allowing
CoroNet to drive high-throughput network flows.

## Disruptor Mapping

`disruptor_t` is the core data-passing layer for `turbo_flow`.

Use `DISRUPTOR_MODE_BROADCAST` when every stage in a dependency graph must observe each message.

Use `DISRUPTOR_MODE_WORKER_POOL` only inside a load-balanced stage group where exactly one worker should process each message.

Mixed graphs should use multiple rings:

```text
broadcast ring
    parse
      |
      v
worker-pool ring
    enrich worker[0..N]
      |
      v
broadcast ring
    persist / metrics / audit
```

The graph compiler must reject invalid topology before starting threads:

- Unknown stage names.
- Cycles.
- Worker-pool dependency misuse.
- Entry size mismatch.
- Missing stage callbacks.
- Ambiguous ownership transfer.

## Relationship to Disruptor Topology API

`turbo_flow` should not replace `disruptor_topology_*()`.

Relationship:

- `disruptor_topology_*()` is the low-level graph builder for one broadcast
  ring.
- `turbo_flow` is a higher-level SDK that parses rules, validates ownership,
  assigns executors, chooses rings, and lowers each broadcast segment to
  `disruptor_topology_*()`.
- Worker-pool and cross-ring completion bridges are owned by `turbo_flow`
  because they span more than one disruptor topology.
- Advanced users can continue using `disruptor_topology_*()` directly when they
  do not need DSL, lifecycle, ownership, or executor integration.

Implementation rule:

- Reuse existing `disruptor_topology_*()` validation and cycle checks wherever
  possible.
- Add only the extra validation that belongs to flow: stage registration,
  executor availability, handoff ownership, and mixed-ring strategy checks.

## MIR Integration

MIR should be an optional backend for expression-heavy stages, not a mandatory runtime dependency.

Planned backends:

```c
typedef enum turbo_flow_expr_backend_e {
    TURBO_FLOW_EXPR_INTERP = 0,
    TURBO_FLOW_EXPR_MIR_JIT,
    TURBO_FLOW_EXPR_AUTO
} turbo_flow_expr_backend_t;
```

MIR candidates:

- Predicate stages.
- Projection stages.
- Numeric transforms.
- Routing conditions.
- Repeated schema-stable message filters.

Compilation policy:

- DSL-declared expressions must be compiled during `turbo_flow_compile()`.
- `turbo_flow_start()` must not leave pending MIR compilation for the first
  message to trigger.
- `TURBO_FLOW_EXPR_AUTO` may choose interpreter vs MIR at compile time based on
  expression shape and platform support.
- Runtime lazy compilation can be revisited only with an explicit warmup API and
  latency benchmarks.

Do not JIT:

- Socket I/O.
- Coroutine scheduling.
- Logging.
- General string manipulation.
- One-shot configuration parsing.

## Tests

Minimum tests:

- Parse a linear graph.
- Parse fan-out and fan-in.
- Parse stage executor syntax for inline, threadpool, coro, coronet, and custom.
- Verify default executor is inline and default data strategy is broadcast.
- Verify `worker N` data strategy and `exec ... workers N` executor config are
  stored independently in IR.
- Reject ambiguous group syntax and require comma-separated group members.
- Reject cycles.
- Reject unknown stages.
- Reject unknown executor kind.
- Reject unavailable `exec coronet` when CoroNet adapter is not linked.
- Reject unregistered custom executor names.
- Reject zero explicit executor worker/lane counts.
- Reject duplicate stage registration.
- Reject compile before all referenced stages are registered.
- Reject register after compile.
- Reject publish before start.
- Reject worker-pool edges in invalid locations.
- Reject two parallel fan-out branches that mutate the same envelope without a
  private-copy boundary.
- Reject ordered + unordered fan-in without a reorder strategy.
- Reject runtime start when MIR expressions were not compiled during compile.
- Execute a linear graph with in-order output.
- Execute a diamond graph with fan-in dependency.
- Execute a worker stage where each message is processed once.
- Verify worker-pool completion can legally reorder messages in MVP.
- Verify unordered worker-pool output cannot be same-sequence fan-in merged with
  ordered output in MVP.
- Verify cross-ring handoff retains and releases `mem_buffer_t`.
- Verify cross-ring handoff moves or deep-copies `owned_payload` and parsed
  ownership without double-free.
- Verify `payload` never outlives its backing buffer.
- Verify transformed `owned_payload` updates `payload` safely.
- Verify executor completion is called exactly once for async executors.
- Verify async executor does not advance the source consumer sequence before
  ownership transfer or completion gate safety.
- Verify coroutine scheduler executor returns completed jobs to
  `turbo_coro_pool_t`.
- Verify coroutine scheduler teardown with live jobs invokes pool discard
  bookkeeping before pool destruction.
- Verify `exec coro` uses Utils `turbo_coro_pool_t` when pool capacity is
  configured.
- Verify `exec threadpool` and worker-pool data strategy can be combined without
  double-consuming messages.
- Verify read-only fan-out stages cannot mutate shared message state through the
  public callback contract.
- Verify `mem_buffer_t` ownership across stage boundaries.
- Verify failed stage status propagation.
- Verify compile errors report code, line, column, and message.
- Verify `turbo_flow_reset(flow, keep_registry)` behavior for both registry
  keeping and full reset.

## Benchmarks

Benchmark separately:

- DSL compile time.
- Pipeline startup time.
- Single-stage publish/consume latency.
- Linear graph throughput.
- Diamond graph throughput.
- Worker-pool throughput with 1, 2, 4, 8 workers.
- Coroutine scheduler executor throughput with and without `turbo_coro_pool_t`.
- Coroutine scheduler executor teardown cost with live and completed jobs.
- MIR expression stage vs interpreter stage.

## Acceptance Criteria

- Existing `disruptor_t` APIs remain source-compatible.
- `turbo_flow` can be disabled at CMake configure time.
- No generated parser files are required at runtime.
- All stage ownership rules are documented in headers.
- Payload view lifetime is closed by tests and documented API contracts.
- Cross-ring handoff has explicit retain/release and backpressure behavior.
- Register, compile, start, publish, stop, and destroy obey a tested lifecycle
  state machine.
- Compile failures expose structured errors with location and message.
- Executor selection is validated during compile, including unavailable
  adapters and unregistered custom executors.
- Data strategy and executor strategy are represented as separate IR fields.
- Flow graph lowering reuses `disruptor_topology_*()` for broadcast segments.
- MVP ordering semantics are documented: broadcast ordered, worker-pool
  unordered by default.
- Broadcast fan-out is read-only by default; mutable branches require exclusive
  ownership, clone/move, or copy-on-write.
- Async executors cannot release source disruptor slots until ownership has been
  transferred safely or stage completion has been gated.
- Coroutine scheduler executor uses Utils `turbo_coro.h` / `turbo_coro_pool.h`
  and does not depend on CoroNet headers.
- CoroNet executor/socket adapters live outside the core and depend on
  `coro_context_t` only at the adapter boundary.
- Ordered fan-in rejects unordered worker-pool branches unless a reorder strategy
  restores sequence identity.
- DSL-declared MIR expressions are compiled before `turbo_flow_start()`, not on
  the first hot-path message.
- `turbo_flow_reset()` defines STOPPED reuse and failed-compile recovery.
- The MVP proves one real parser/filter workload before adding general code generation.
