# Turbo LangChain

`turbo_langchain` is the reusable library layer for graph-driven LLM workflows in TurboNet.
It contains the runtime and data-model pieces that should stay portable across desktop,
server, and embedded-friendly native builds:

- graph execution and checkpoints
- prompt/message builders
- model provider selection
- tool registries and schemas
- host-agnostic tool runtimes and registry bridges
- action-tool bridges
- OpenAI-compatible agent runtime

It does not include shell execution, builtin file tools, or engineering-loop application
helpers. Those live in `agent/` and are exposed through the `turbo_agent` target.

## Long-term aim

`turbo_langchain` is intended to grow into a reusable agent kernel for coding
and operator-style hosts, closer in spirit to tools such as OpenClaw or Claude
Code than to a one-off demo loop.

The target shape is:

- host-agnostic workflow runtime in `langchain/`
- stable action and tool contracts instead of CLI-specific behavior
- cross-platform tool execution through pluggable runtimes
- schema-driven runtime data bind for context, tool payloads, and checkpoints
- native and sandboxed backends, including DLL-style native tools and wasm3-based tools
- policy, trace, checkpoint, review, and replan semantics that stay stable across hosts

This module should own the reusable semantics. Concrete CLIs, shells, approval
UX, and product-specific orchestration remain outside it.

## CMake targets

- `turbo_langchain`
- `TurboNet::LangChain`

Compatibility targets kept for existing users:

- `turbo_graph`
- `TurboNet::Graph`

## Public headers

For a one-shot include, use:

```c
#include "turbo_langchain.h"
```

Or include only the narrower headers you need, such as `turbo_graph.h` or
`turbo_openai_agent.h`.

`turbo_prompt.h` now exposes parallel bind-native message and template helpers,
and `turbo_tool_registry.h` exposes bind-native tool execution. That lets hosts
keep prompt input, tool arguments, and tool results on one runtime value model
instead of bouncing through ad hoc JSON strings.
It also exposes canonical bind-native message schemas and validation helpers, so
upper layers can depend on one explicit message contract instead of implicit
`role/content` object conventions.

For host-agnostic tool execution and `tool_registry` bridging, use:

```c
#include "turbo_tool_runtime.h"
```

That layer defines a small vtable-based runtime contract so native callbacks,
shared-library loaders, and future wasm3-backed tool hosts can converge on one
execution surface.

For the optional wasm3-backed runtime backend, use:

```c
#include "turbo_tool_runtime_wasm3.h"
```

That backend loads guest modules through TurboNet's wasm3 wrapper and maps a
small exported tool ABI back into the same `turbo_tool_runtime_t` surface.
The generic `turbo_tool_runtime_default_create(...)` entry now resolves to this
wasm3 backend; hosts that want in-process callbacks must opt into the native
runtime explicitly.

For schema-driven runtime value binding, use:

```c
#include "turbo_runtime_data_bind.h"
```

That layer exposes a small host-neutral value tree plus a builder vtable meant
for future schema codecs. The default implementation is in-memory and portable;
future MIR-backed codecs should target that vtable instead of baking host
details into public APIs. `turbo_chain` and `turbo_graph` now also expose small
bind-boundary helpers so hosts can keep JSON as an implementation detail rather
than a required application data model.

For binary wire parsing primitives, use:

```c
#include "turbo_runtime_binary_reader.h"
```

That layer defines the canonical little-endian cursor and var-string rules the
future binary codec should reuse for validation, interpreter fallback, and MIR
JIT generation. The codec should compile against one wire contract, not hide
multiple ad hoc readers in different backends.

For declarative binary layouts and ABI planning, use:

```c
#include "turbo_runtime_binary_schema.h"
```

That layer describes canonical binary object fields, validates payloads against
the wire contract, and collects the exact
`turbo_runtime_data_bind_value_api_t` callbacks a future MIR parser will need.

For MIR-backed binary parser planning and JIT stubs, use:

```c
#include "turbo_runtime_binary_mir.h"
```

That layer turns binary schema requirements into a stable external-symbol plan
and can JIT a parser entry against the vendored MIR runtime. The current
implementation has three specialized fast paths plus one conservative fallback:

- all scalar fields: field-by-field MIR assembly over one shared reader
- all repeated-scalar fields: MIR skeleton plus one repeated-value bridge per field
- all string-key-map scalar fields: MIR skeleton plus one map-value bridge per field
- mixed schemas or nested objects: validated interpreter fallback through one stable
  parser entry

This keeps one real parser surface available now while MIR specialization grows
incrementally behind the same ABI contract.

The OpenAI-agent surface is layered from low to high as:

- `turbo_openai_agent.h` for core config and lifecycle
- `turbo_openai_agent_extensions.h` for optional middleware, trace, store,
  guardrail, and runnable helpers
- `turbo_openai_agent_state.h` for state inspection and mutation
- `turbo_openai_agent_sse.h` for SSE aggregation helpers
- `turbo_openai_agent_graph.h` for graph nodes and predicates
- `turbo_openai_agent_workflow.h` for canned workflow installers

Agent-state inspection and mutation helpers live in:

```c
#include "turbo_openai_agent_state.h"
```

That keeps state-schema access separate from graph topology and workflow
presets, while `turbo_openai_agent.h` still re-exports it for compatibility.
The state helper layer now also exposes bind-boundary creation and snapshot
helpers so hosts can inspect workflow/control state without adopting the JSON
tree as their own application model.

SSE stream aggregation helpers live in:

```c
#include "turbo_openai_agent_sse.h"
```

That keeps wire-format reconstruction separate from the runtime agent lifecycle
surface, while `turbo_openai_agent.h` still re-exports it for compatibility.

Optional middleware, trace, store, guardrail, and runnable helpers live in:

```c
#include "turbo_openai_agent_extensions.h"
```

That keeps lifecycle/configuration APIs separate from optional runtime
extension points, while `turbo_openai_agent.h` still re-exports them for
compatibility.

Workflow graph helpers for the OpenAI agent live in:

```c
#include "turbo_openai_agent_graph.h"
```

That keeps the graph-node and edge-predicate surface out of the base runtime
header.

Workflow installer presets live one step higher:

```c
#include "turbo_openai_agent_workflow.h"
```

Use that header only when you want the canned loop topologies.

## Link model

```cmake
target_link_libraries(my_app PRIVATE TurboNet::LangChain)
```

If you need builtin shell/file tools or engineering-agent helpers, link
`TurboNet::Agent` instead.

## Cross-platform intent

The library layer keeps platform-sensitive behavior narrow:

- path policy is case-sensitive on non-Windows hosts
- Windows path separators are normalized only on Windows
- tool execution should converge on host-neutral contracts with backend vtables
- runtime data binding should converge on host-neutral value builders and schemas
- binary codec work should share one validated wire reader before MIR specialization
- binary schema work should collect one explicit ABI contract before MIR emission
- MIR codegen should target one stable extern plan instead of ad hoc callback wiring
- backend implementations may use native loaders or wasm3-based sandboxes
- cross-platform utilities such as filesystem and platform helpers should be reused
  rather than reimplemented inside agent hosts
- application CLI integration tests stay outside this module

That keeps `langchain/` focused on library semantics rather than host tooling.
