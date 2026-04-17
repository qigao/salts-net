# Runtime V2

`Runtime V2` does not replace `Runtime V1`. It adds one thin host-facing layer
above it.

`Runtime V1` solved durable execution:

- `thread`
- `run`
- `checkpoint`
- `resume`
- `fork`
- `history`

`Runtime V2` solves two remaining host problems:

- repeated runtime bootstrap glue
- ad hoc long-term memory key naming
- packaging one configured app/session as a reusable tool
- read-only thread lineage / branch inspection without re-implementing runtime queries

The current `Runtime V3` staging work builds on that base without changing the
durable runtime store contract. The first orchestration foundation is:

- one canonical `supervisor_versions` state lane for `active_agent`,
  `target_agent`, `handoff_reason`, `inbox`, and `handoff_history`
- one minimal `turbo_agent_install_supervisor_loop(...)` installer that adds
  supervisor/handoff route nodes while leaving concrete agent nodes to the host

## New public headers

Include:

```c
#include "turbo_agent_session.h"
#include "turbo_agent_memory_store.h"
#include "turbo_agent_subagent.h"
#include "turbo_agent_runtime_remote.h"
```

Or use the aggregate include:

```c
#include "turbo_langchain.h"
```

## Agent session

`turbo_agent_session_t` is a thin wrapper around:

- one `turbo_agent_runtime_t`
- one optional `turbo_agent_t`
- one optional `turbo_agent_memory_store_t`
- remembered `thread_id`
- remembered latest `run_id`
- remembered latest non-null `checkpoint_id`

It exists to remove host repetition, not to invent new runtime semantics.

The underlying runtime surface will also expose
`turbo_agent_runtime_get_thread_timeline_bind(...)` and
`turbo_agent_runtime_list_thread_lineage(...)`; the session and app wrappers
keep the same timeline shape and lineage shape, only with their own remembered
thread context.

### Session config

`turbo_agent_session_config_t` contains:

- `runtime_store`
- `memory_store`
- `agent_config`
- `thread_id`
- `env_path`
- `load_env`
- `overwrite_env`
- `workflow_kind`
- `memory_namespace`
- `parent_agent_run_id`
- `parent_tool_call_id`
- `parent_tool_name`

When `load_env` is non-zero, the session applies:

```c
turbo_agent_config_apply_env(...)
```

That path already reuses the parser layer's:

- `turbo_dotenv_load(...)`
- `turbo_dotenv_load_default(...)`

before creating the agent.

This means real work can pick up:

- `OPENAI_API_KEY`
- `OPENAI_BASE_URL`
- `OPENAI_MODEL`
- `OPENAI_PROVIDER`

from `.env` without making every host duplicate the same bootstrap code.

### Session API

- `turbo_agent_session_create(...)`
- `turbo_agent_session_destroy(...)`
- `turbo_agent_session_agent(...)`
- `turbo_agent_session_runtime(...)`
- `turbo_agent_session_memory_store(...)`
- `turbo_agent_session_workflow_kind(...)`
- `turbo_agent_session_memory_namespace(...)`
- `turbo_agent_session_model(...)`
- `turbo_agent_session_base_url(...)`
- `turbo_agent_session_provider_name(...)`
- `turbo_agent_session_has_api_key(...)`
- `turbo_agent_session_thread_id(...)`
- `turbo_agent_session_last_run_id(...)`
- `turbo_agent_session_last_checkpoint_id(...)`
- `turbo_agent_session_get_thread(...)`
- `turbo_agent_session_get_run(...)`
- `turbo_agent_session_get_checkpoint(...)`
- `turbo_agent_session_get_thread_state_bind(...)`
- `turbo_agent_session_get_run_state_bind(...)`
- `turbo_agent_session_get_checkpoint_state_bind(...)`
- `turbo_agent_session_get_child_run(...)`
- `turbo_agent_session_get_child_checkpoint(...)`
- `turbo_agent_session_list_runs(...)`
- `turbo_agent_session_list_child_runs(...)`
- `turbo_agent_session_list_checkpoints(...)`
- `turbo_agent_session_list_thread_lineage(...)`
- `turbo_agent_session_get_thread_timeline_bind(...)`
- `turbo_agent_session_load_history_events_bind(...)`
- `turbo_agent_session_load_child_history_events_bind(...)`
- `turbo_agent_session_apply_command_bind(...)`
- `turbo_agent_session_resume_command_bind(...)`
- `turbo_agent_session_fork_command_bind(...)`
- `turbo_agent_session_start_bind_graph(...)`
- `turbo_agent_session_resume_bind_graph(...)`
- `turbo_agent_session_fork_bind_graph(...)`
- `turbo_agent_session_create_loop_graph(...)`
- `turbo_agent_session_create_review_graph(...)`
- `turbo_agent_session_create_engineering_graph(...)`
- `turbo_agent_session_create_preset_graph(...)`
- `turbo_agent_session_create_input_state_bind(...)`
- `turbo_agent_session_create_input_messages_state_bind(...)`
- `turbo_agent_session_start_preset_bind_graph(...)`
- `turbo_agent_session_start_preset_text(...)`
- `turbo_agent_session_start_text(...)`
- `turbo_agent_session_start_preset_messages(...)`
- `turbo_agent_session_start_messages(...)`
- `turbo_agent_session_result_text(...)`
- `turbo_agent_session_invoke_preset_text(...)`
- `turbo_agent_session_invoke_text(...)`
- `turbo_agent_session_invoke_preset_messages_text(...)`
- `turbo_agent_session_invoke_messages_text(...)`
- `turbo_agent_session_invoke_preset_json(...)`
- `turbo_agent_session_invoke_json(...)`
- `turbo_agent_session_invoke_preset_messages_json(...)`
- `turbo_agent_session_invoke_messages_json(...)`
- `turbo_agent_session_memory_get(...)`
- `turbo_agent_session_memory_put(...)`
- `turbo_agent_session_memory_put_context(...)`
- `turbo_agent_session_memory_delete(...)`
- `turbo_agent_session_memory_list(...)`
- `turbo_agent_session_memory_list_records(...)`
- `turbo_agent_session_memory_query_records(...)`
- `turbo_agent_session_load_memory_context(...)`
- `turbo_agent_session_create_input_state_with_memory_bind(...)`
- `turbo_agent_session_create_input_messages_state_with_memory_bind(...)`
- `turbo_agent_session_start_preset_text_with_memory(...)`
- `turbo_agent_session_resume_preset_bind_graph(...)`
- `turbo_agent_session_fork_preset_bind_graph(...)`

### Session behavior

The session does one useful thing on top of raw runtime calls:

- after `start`, it remembers thread/run/checkpoint ids
- after `resume`, it updates thread/run and keeps the last non-null checkpoint
- after `fork`, it updates thread/run and still keeps the last source checkpoint

That means callers may pass `NULL` for `checkpoint_id` on resume/fork and let
the session use the last remembered checkpoint.

The same wrapper rule now exists for runtime inspection:

- `get_run(NULL, ...)` resolves to the latest remembered run
- `get_checkpoint(NULL, ...)` resolves to the latest remembered checkpoint
- `get_run_state_bind(NULL, ...)` resolves to the latest remembered run
- `get_checkpoint_state_bind(NULL, ...)` resolves to the latest remembered checkpoint
- `list_checkpoints(NULL, ...)` resolves to checkpoints for the latest remembered run
- `load_history_events_bind(NULL, NULL, ...)` resolves to the latest remembered
  run/checkpoint pair
- `turbo_agent_session_get_thread_timeline_bind(...)` resolves the latest
  remembered thread and returns one timeline bundle with `thread`,
  `resolved_current_run`, `resolved_current_checkpoint_id`,
  `resolved_current_checkpoint`, `latest_run`, `pending_run`, `runs`,
  `current_run_checkpoints`, and `history_events`

`resolved_current_checkpoint` is a lightweight summary for the current thread
head. Full checkpoint records still come from `get_checkpoint(...)`.
That lightweight summary is intentionally small and stable:
`id`, `run_id`, `parent_checkpoint_id`, `status`, `created_at`, `next_node`,
`seq`, and numeric `steps`.

For an interrupted thread, hosts should treat the timeline bundle like this:

```json
{
  "thread": {"id": "thr_..."},
  "resolved_current_run": {"id": "run_interrupt"},
  "resolved_current_checkpoint_id": "ckpt_review",
  "resolved_current_checkpoint": {
    "id": "ckpt_review",
    "run_id": "run_interrupt",
    "parent_checkpoint_id": null,
    "status": "interrupted",
    "created_at": "2026-04-16T12:00:00Z",
    "next_node": "review",
    "seq": 1,
    "steps": 1
  },
  "latest_run": {"id": "run_interrupt"},
  "pending_run": {"id": "run_interrupt"},
  "runs": [{"id": "run_interrupt"}],
  "current_run_checkpoints": [{"id": "ckpt_review"}],
  "history_events": [{"type": "node_start"}]
}
```

The important part is the selection rule, not the literal ids: for interrupted
threads, `resolved_current_run` and `pending_run` point at the interrupted run,
and `resolved_current_checkpoint` is always the lightweight summary for that
head checkpoint.

`turbo_agent_session_list_thread_lineage(...)` is a separate read-only view for
browser-style inspection:

- it returns lineage for one thread without changing state
- it does not change `resume_bind_graph(...)` / `fork_bind_graph(...)`
  semantics
- it is meant for host/UI lineage browsers, not for execution control
- the payload includes:
- `thread_id`
- `latest_run_id`
- `pending_run_id`
- `root_checkpoint_id`
- `branches`
- each `branches[]` entry includes:
- `run_id`
- `checkpoint_id`
- `branch_root_checkpoint_id`
- `parent_checkpoint_id`
- `status`
- `updated_at`

The same defaulting rule also now applies to command application:

- `apply_command_bind(NULL, command, ...)` resolves to the latest remembered
  checkpoint before producing a fresh `state_override`
- `resume_command_bind(NULL, command, ...)` resolves the latest remembered
  checkpoint, applies the command, then resumes in one call
- `fork_command_bind(NULL, command, ...)` resolves the latest remembered
  checkpoint, applies the command, then forks in one call
- the runtime also exposes thread-scoped command helpers, so hosts can start
  from one `thread_id` and let the runtime resolve the pending run and latest
  checkpoint internally before command application
- `resume_checkpoint_bind_graph(...)` / `fork_checkpoint_bind_graph(...)`
  are the explicit checkpoint-scoped replay aliases; they continue or fork
  directly from one concrete historical checkpoint
- `apply_checkpoint_command_bind(...)`,
  `resume_checkpoint_command_bind(...)`, and
  `fork_checkpoint_command_bind(...)` are the matching explicit
  checkpoint-scoped command aliases; they keep command application pinned to
  one concrete historical checkpoint
- `turbo_agent_runtime_get_checkpoint_context(...)` /
  `turbo_agent_session_get_checkpoint_context(...)` /
  `turbo_agent_app_get_checkpoint_context(...)` return one historical
  checkpoint as a read-only aggregate bundle with `checkpoint_summary`,
  `state`, `run`, `thread`, and `history_events`; the result is a context
  inspect view, not a replacement for `get_checkpoint(...)`
- `resume_thread_bind_graph(...)` / `fork_thread_bind_graph(...)` do the same
  for replay: they resolve the thread's current checkpoint internally, then
  forward the caller's state override into the existing resume/fork path
- `update_checkpoint_state_bind(...)` / `update_thread_state_bind(...)`
  formalize the low-level state-edit surface: they merge one bind-native state
  patch into the resolved checkpoint state and return one full
  `state_override`, without mutating persisted checkpoint records in place
- `resume_checkpoint_state_bind_graph(...)` /
  `fork_checkpoint_state_bind_graph(...)` and their thread-scoped variants
  collapse state patch plus resume/fork into one host-facing time-travel call
- completed-only threads have no current checkpoint, so the thread-scoped
  replay helpers fail explicitly instead of inventing one
- `get_latest_run(thread_id)` resolves the newest run on one thread by
  `updated_at`
- `get_pending_run(thread_id)` resolves the newest interrupted run on one
  thread
- `get_latest_checkpoint(run_id)` resolves `run.latest_checkpoint_id` without
  forcing the host to read and unpack the run record itself
- `turbo_agent_session_get_thread_timeline_bind(thread_id)` resolves the thread
  timeline with pending run first, otherwise latest run, so hosts do not need
  to make that choice themselves
- `load_thread_history_events_bind(thread_id)` prefers the newest interrupted
  run on the thread, then falls back to the newest run, and replays that run's
  durable history in one call
- `replay_history_bind(...)` / `replay_thread_history_bind(...)` do not define
  a second observer event model; they replay the same durable history objects
  through the existing `turbo_event_sink_bind_fn` callback boundary
- `observe_history_bind(...)` / `observe_thread_history_bind(...)` are the
  host-facing observer bridge for that same durable history. They reuse the
  existing history facts and narrow them into one observer family:
  `model_delta`, `tool_call_started`, `tool_result`, `state_updated`,
  `interrupted`, `completed`. Each emitted observer object carries
  `kind="observer"`, one narrowed `type`, and the original canonical event
  clone under `event`.
- `turbo_agent_session_add_trace_bind_sink(...)` /
  `turbo_agent_app_add_trace_bind_sink(...)` are the live-side bridge for the
  same canonical trace event shape, and
  `*_set_trace_history_enabled(...)` lets hosts persist those trace events into
  run state for later checkpoint or thread inspection
- `turbo_agent_session_add_observer_bind_sink(...)` /
  `turbo_agent_app_add_observer_bind_sink(...)` are the live-side observer
  counterparts. They sit above the existing trace stream and do not create a
  second persisted observer log.
- `get_*_trace_events_bind(...)` is the durable snapshot convenience layer for
  the same data. It is intentionally derived from persisted state, not a second
  observer log, and returns an empty array when no trace history exists yet.
- `get_thread_observability_index(...)` and the session/app wrappers are the
  matching thread-scoped observability bundle: they aggregate the existing
  `thread`, `latest_run`, `pending_run`, `thread_timeline`, `thread_lineage`,
  `branch_tree`, `history_events`, and `trace_events` facts, then derive
  top-level quick-read fields such as `current_status`,
  `current_interrupt_reason`, `current_pending_action`,
  `current_checkpoint_summary`, `latest_run_status`, `latest_run_updated_at`,
  `pending_run_id`, `pending_checkpoint_id`, `has_failure`,
  `has_model_error`, `has_guardrail_rejection`, `replan_requested`,
  `current_failure_reason`, `current_review_note`, `has_pending_review`,
  `has_handoff`, `active_agent`, and one `counts` object on top. They do not
  add a new persisted log or a second store.
- `list_observability_indexes(...)` is the runtime-only cross-thread
  counterpart: it returns one narrowed observability summary per persisted
  thread, sorted by thread `updated_at` descending, while session/app remain
  current-thread scoped.
- `list_observability_indexes_filtered(...)` is the matching runtime-only
  filter layer for cross-thread lists. The current filter keys are `status`,
  `has_pending_review`, `has_failure`, `has_handoff`, `active_agent`,
  `has_model_error`, `has_guardrail_rejection`, `replan_requested`,
  `current_interrupt_reason`, `latest_run_status`,
  `latest_run_updated_after`, `latest_run_updated_before`, and
  `thread_id_prefix`; it also supports `sort_by`, `sort_order`, and `limit`.
  The default ordering remains `latest_run_updated_at desc`; `sort_by=thread_id`
  defaults to `asc`.
- `turbo_agent_runtime_remote_dispatch_jsonrpc(...)` is one transport-agnostic
  local JSON-RPC 2.0 dispatcher over that same runtime surface. The current
  method set currently includes `runtime.start`, `runtime.resume`,
  `runtime.fork`, `runtime.applyCommand`,
  `runtime.resumeThreadCommandBindGraph`,
  `runtime.forkThreadCommandBindGraph`,
  `runtime.getThreadState`, `runtime.updateThreadState`,
  `runtime.applyThreadStatePatch`,
  `runtime.resumeThreadBindGraph`, `runtime.forkThreadBindGraph`,
  `runtime.getCheckpointContext`,
  `runtime.resumeThreadStatePatchBindGraph`,
  `runtime.forkThreadStatePatchBindGraph`,
  `runtime.getThreadObservabilityIndex`, and
  `runtime.listObservabilityIndexesFiltered`. It exists so a future HTTP or
  RPC adapter can reuse one stable runtime contract instead of re-encoding
  calls ad hoc.
- `turbo_agent_runtime_remote_dispatch_jsonrpc_text(...)` is the transport-
  neutral raw JSON text adapter over that same dispatcher.
- `turbo_agent_runtime_remote_handle_http_jsonrpc(...)` is the current HTTP-
  like adapter for `POST /v1/runtime/jsonrpc`. It still does not start a
  listener or own a server; it only maps adapter-level path/method/body checks
  to HTTP status while keeping JSON bodies aligned with the same runtime
  contract.
- `turbo_agent_runtime_remote_iris_mount(...)` is the current Iris adapter over
  that same bridge. It binds by app/path pair through Iris's RPC endpoint
  registry, so it can now coexist with native `rpc_setup_endpoint(...)` routes
  on the same `iris_app_t` as long as endpoint paths differ.

The dispatcher does not define a second runtime result shape. It keeps the
existing runtime payloads and wraps them in JSON-RPC 2.0 envelopes:

```json
{
  "jsonrpc": "2.0",
  "id": "req-start",
  "method": "runtime.start",
  "params": {
    "graph_name": "remote-skeleton",
    "thread_id": "remote-thread-1",
    "state": {},
    "options": { "interrupt_before_nodes": ["end"] }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "req-start",
  "result": {
    "summary": {
      "thread_id": "remote-thread-1",
      "status": "interrupted",
      "run_id": "run_...",
      "checkpoint_id": "ckpt_..."
    },
    "state": { "visited_start": true, "visited_end": false }
  },
  "error": null
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "req-context",
  "method": "runtime.getCheckpointContext",
  "params": { "checkpoint_id": "ckpt_..." }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "req-context",
  "result": {
    "context": {
      "checkpoint_summary": { "id": "ckpt_..." },
      "run": { "id": "run_..." },
      "thread": { "id": "remote-thread-1" },
      "state": {},
      "history_events": []
    }
  },
  "error": null
}
```

The current minimum JSON-RPC error matrix is:
- `-32600`: invalid request
- `-32601`: method not found
- `-32602`: invalid params
- `-32603`: internal runtime or dispatch failure

`-32600` means the JSON-RPC envelope itself is malformed. `-32602` means the
envelope is valid but the selected runtime method receives malformed or
missing params.

The stable remote wrapper contract is locked by these golden fixtures:
- `runtime_remote_thread_control.golden.json`
- `runtime_remote_graph_runs.golden.json`
- `runtime_remote_inspect.golden.json`
- `runtime_remote_errors.golden.json`
- `runtime_remote_invalid_params.golden.json`

The invalid-params golden set intentionally covers both simple inspect methods
and graph-bound/thread-bound methods so the dispatcher keeps one stable
parameter-validation contract.

Where params are stable, the thread-control / graph-run / inspect golden sets
also lock canonical request shapes, not only success envelopes. That keeps
`graph_name` / `thread_id` / `checkpoint_id` / `command` / `state` /
`state_patch` payload shapes aligned with the dispatcher implementation.

Graph-bound success responses are also golden-locked so the dispatcher keeps
one stable `summary/state` envelope for resume/fork and thread-scoped replay
methods.
- `get_child_trace_events_bind(...)` is the child-lineage counterpart: it reads
  trace history from the resolved child checkpoint when present, otherwise from
  the child run snapshot, without adding a new lineage factsource.
- `get_child_checkpoint_context(...)` is the one-shot inspect counterpart when
  the parent output item already carries `child_checkpoint_id`.
- `get_child_thread_timeline_bind(...)` is the one-shot child-thread timeline
  counterpart when the parent output item already carries `child_thread_id`.
- `get_child_branch_tree(...)` is the one-shot child-thread branch-tree
  counterpart when the parent output item already carries `child_thread_id`.
- `list_child_checkpoints(...)` is the child-run list counterpart when the
  parent output item already carries `child_run_id`.
- `get_child_inspect(...)` is the host-facing child detail bundle: it reuses the
  same child lineage facts to return `run`, `checkpoints`, optional
  `latest_checkpoint`, optional `checkpoint_context`, optional
  `thread_timeline`, optional `branch_tree`, and child `history_events` /
  `trace_events` in one read-only object.
- `get_child_orchestration_inspect(...)` is the parent/child orchestration
  counterpart on session/app: it keeps `child_inspect` intact and adds
  `parent_agent_run_id`, `parent_tool_call_id`, and `parent_tool_name` as the
  parent lineage wrapper.
- `get_child_multi_agent_inspect(...)` is the higher-level multi-agent
  aggregate on session/app: it reuses the current thread's
  `supervisor_inspect` / `orchestration_inspect` and the resolved
  `child_orchestration_inspect`, without creating a new runtime persistence
  surface.
- `get_supervisor_inbox(...)` / `get_supervisor_handoff_history(...)` are the
  control-state mailbox readers on session/app. They are derived from the
  current thread state and intentionally do not create a second runtime store
  collection.
- `get_supervisor_inspect(...)` is the matching one-shot control-state inspect
  bundle on session/app. It returns `supervisor`, `inbox`,
  `handoff_history`, `control_snapshot`, and `workflow_snapshot` in one
  read-only object.
- `get_orchestration_inspect(...)` is the next session/app aggregate layer:
  it bundles `supervisor_inspect`, `thread_timeline`, `thread_lineage`,
  `branch_tree`, and `child_runs` in one read-only multi-agent inspect object
  without introducing a new runtime persistence layer.
- `append_supervisor_inbox_message_bind(...)` is the matching write-side
  mailbox helper: it returns one state override bind value that the host can
  feed into the existing replay/resume/fork surfaces.
- interrupted summaries now expose `interrupt_reason`, `pending_node`,
  `pending_action`, legacy `available_commands`, and structured
  `available_command_descriptors` so hosts do not need to infer the next UI
  action from raw state alone

This is not a new special case. It removes a host-side special case.

The descriptor metadata is host-facing only: it helps a UI choose how to
present a command, but the runtime still resolves and executes the legacy
command name exactly as before.

When `workflow_kind` and optional `memory_namespace` are set on the session
config, the default wrappers:

- `turbo_agent_session_start_text(...)`
- `turbo_agent_session_start_messages(...)`
- `turbo_agent_session_invoke_text(...)`
- `turbo_agent_session_invoke_messages_text(...)`
- `turbo_agent_session_invoke_json(...)`
- `turbo_agent_session_invoke_messages_json(...)`

let the host act much closer to a `create_agent(...).invoke(...)` surface
without restating the same preset and memory namespace on every call, whether
the host begins from one user string or one canonical message array.

### App API

`turbo_agent_app_t` is one deliberately thin wrapper above session:

- `turbo_agent_app_create(...)`
- `turbo_agent_app_destroy(...)`
- `turbo_agent_app_session(...)`
- `turbo_agent_app_thread_id(...)`
- `turbo_agent_app_last_run_id(...)`
- `turbo_agent_app_last_checkpoint_id(...)`
- `turbo_agent_app_workflow_kind(...)`
- `turbo_agent_app_memory_namespace(...)`
- `turbo_agent_app_memory_store(...)`
- `turbo_agent_app_get_thread(...)`
- `turbo_agent_app_get_run(...)`
- `turbo_agent_app_get_checkpoint(...)`
- `turbo_agent_app_get_thread_state_bind(...)`
- `turbo_agent_app_get_run_state_bind(...)`
- `turbo_agent_app_get_checkpoint_state_bind(...)`
- `turbo_agent_app_get_child_run(...)`
- `turbo_agent_app_get_child_checkpoint(...)`
- `turbo_agent_app_list_runs(...)`
- `turbo_agent_app_list_child_runs(...)`
- `turbo_agent_app_list_checkpoints(...)`
- `turbo_agent_app_list_thread_lineage(...)`
- `turbo_agent_app_get_thread_timeline_bind(...)`
- `turbo_agent_app_load_history_events_bind(...)`
- `turbo_agent_app_replay_history_bind(...)`
- `turbo_agent_app_replay_thread_history_bind(...)`
- `turbo_agent_app_load_child_history_events_bind(...)`
- `turbo_agent_app_apply_command_bind(...)`
- `turbo_agent_app_resume_command_bind(...)`
- `turbo_agent_app_fork_command_bind(...)`
- `turbo_agent_app_resume_preset_bind_graph(...)`
- `turbo_agent_app_fork_preset_bind_graph(...)`
- `turbo_agent_app_resume_preset_command_bind(...)`
- `turbo_agent_app_fork_preset_command_bind(...)`
- `turbo_agent_app_start_text(...)`
- `turbo_agent_app_start_messages(...)`
- `turbo_agent_app_invoke_text(...)`
- `turbo_agent_app_invoke_messages_text(...)`
- `turbo_agent_app_invoke_json(...)`
- `turbo_agent_app_invoke_messages_json(...)`
- `turbo_agent_app_memory_get(...)`
- `turbo_agent_app_memory_put(...)`
- `turbo_agent_app_memory_put_context(...)`
- `turbo_agent_app_memory_delete(...)`
- `turbo_agent_app_memory_list(...)`

This layer does not invent a second runtime. It removes one more host-side
special case by packaging a configured session as a more direct
`create_agent(...).invoke(...)`-style surface.

The app-level `turbo_agent_app_get_thread_timeline_bind(...)` wrapper follows
the same resolved-current-run rule as the session/runtime layer: pending run
first, otherwise latest run.

`turbo_agent_app_list_thread_lineage(...)` follows the same read-only lineage
inspection contract as the runtime/session layer. The same browser-facing
inspect surface is also exposed as:

- `turbo_agent_runtime_get_branch_tree(...)`
- `turbo_agent_session_get_branch_tree(...)`
- `turbo_agent_app_get_branch_tree(...)`

It is for time-travel and lineage browsing only; it does not alter resume,
fork, or checkpoint replay behavior. The minimum payload should include:

- `thread_id`
- `current_run_id`
- `latest_run_id`
- `pending_run_id`
- `current_checkpoint_id`
- `current_checkpoint_summary`
- `current_branch`
- `branches`
- `edges`

`branches` carries the branch nodes for the current thread or lineage root.
`edges` describes lineage links between checkpoints and runs. For fork edges,
the direction is always from the source checkpoint to the child run created
from that checkpoint. `pending_run_id` only reflects the current branch head:
it is present when the latest/current run is interrupted, and null when only
an older side branch remains pending. `current_branch` is the branch node for
`current_run_id`, and `current_checkpoint_id` is that branch head's checkpoint
id when one exists. `current_checkpoint_summary` is the lightweight summary for
that checkpoint. Branch nodes may also carry `branch_root_checkpoint_id` as the
stable entry checkpoint anchor for that branch. Branch nodes also carry
`checkpoint_summary` and `source_checkpoint_summary`, and lineage edges carry
`source_checkpoint_summary`; these are lightweight inspect views, not full
records, and they do not replace `get_checkpoint(...)`.
Those checkpoint summaries use the same stable field set:
`id`, `run_id`, `parent_checkpoint_id`, `status`, `created_at`, `next_node`,
`seq`, and numeric `steps`.

For a forked thread, the minimum branch-tree shape should look like this:

```json
{
  "thread_id": "thr_...",
  "current_run_id": "run_fork",
  "latest_run_id": "run_fork",
  "pending_run_id": null,
  "current_checkpoint_id": null,
  "current_checkpoint_summary": null,
  "current_branch": {
    "run_id": "run_fork",
    "parent_run_id": "run_base",
    "source_checkpoint_id": "ckpt_review",
    "branch_root_checkpoint_id": "ckpt_review",
    "checkpoint_id": null,
    "checkpoint_summary": null,
    "source_checkpoint_summary": {
      "id": "ckpt_review",
      "run_id": "run_base",
      "parent_checkpoint_id": null,
      "status": "interrupted",
      "created_at": "2026-04-16T12:00:00Z",
      "next_node": "review",
      "seq": 1,
      "steps": 1
    }
  },
  "branches": [
    {
      "run_id": "run_base",
      "parent_run_id": null,
      "source_checkpoint_id": null,
      "checkpoint_id": "ckpt_review",
      "checkpoint_summary": {
        "id": "ckpt_review",
        "run_id": "run_base",
        "parent_checkpoint_id": null,
        "status": "interrupted",
        "created_at": "2026-04-16T12:00:00Z",
        "next_node": "review",
        "seq": 1,
        "steps": 1
      },
      "source_checkpoint_summary": null,
      "branch_root_checkpoint_id": "ckpt_review"
    },
    {
      "run_id": "run_fork",
      "parent_run_id": "run_base",
      "source_checkpoint_id": "ckpt_review",
      "checkpoint_id": null,
      "checkpoint_summary": null,
      "source_checkpoint_summary": {
        "id": "ckpt_review",
        "run_id": "run_base",
        "parent_checkpoint_id": null,
        "status": "interrupted",
        "created_at": "2026-04-16T12:00:00Z",
        "next_node": "review",
        "seq": 1,
        "steps": 1
      },
      "branch_root_checkpoint_id": "ckpt_review"
    }
  ],
  "edges": [
    {
      "kind": "fork",
      "source_checkpoint_id": "ckpt_review",
      "target_run_id": "run_fork",
      "source_checkpoint_summary": {
        "id": "ckpt_review",
        "run_id": "run_base",
        "parent_checkpoint_id": null,
        "status": "interrupted",
        "created_at": "2026-04-16T12:00:00Z",
        "next_node": "review",
        "seq": 1,
        "steps": 1
      }
    }
  ]
}
```

Hosts should read `edges[]` as the durable fork relation and treat
`branch_root_checkpoint_id` as the stable anchor for a branch, instead of
reconstructing those relations from raw checkpoint chains themselves.

`turbo_agent_*_get_checkpoint_context(...)` is the matching one-shot inspect
surface for a single historical checkpoint. It bundles the lightweight
`checkpoint_summary`, serialized `state`, its run, its thread, and
`history_events` so hosts can render a checkpoint detail panel without
reassembling those records themselves.

The README now includes compact golden examples for checkpoint context, thread
timeline, branch tree, command descriptors, and the newer multi-agent
session/app inspect bundles. Those examples mirror the stable field sets this
runtime section describes.

For review-style interrupts, the host now has two valid paths:

- keep `apply_command_bind(...)` when it wants to inspect or persist the
  produced override state itself
- use `resume_command_bind(...)` / `fork_command_bind(...)` when it only wants
  a one-shot "command then continue" path
- use `resume_preset_command_bind(...)` / `fork_preset_command_bind(...)` when
  it wants that same one-shot path on the configured preset workflow surface

`available_command_descriptors` is now the preferred host-facing contract. Each
descriptor carries:

- `name`
- `label`
- `description`
- `resume_mode`
- `args_schema`
- optional host/UI metadata like `category`, `input_mode`,
  `requires_input`, `suggested_title`, `primary_key`, `accepted_keys`,
  `fallback_keys`, `supports_json_value`, `placeholder`,
  `example_payload`, and `success_state_hint`

`available_commands` remains as a legacy string array for compatibility.

The descriptor shape is intentionally small and host-oriented. Hosts should use
these fields to render action panels and input forms; they do not alter the
legacy `available_commands` list. A review gate now looks like this on the
no-input side:

```json
{
  "name": "approve_review",
  "label": "Approve Review",
  "suggested_title": "Approve Review",
  "description": "Approve pending review and continue the interrupted run.",
  "category": "review",
  "input_mode": "none",
  "placeholder": null,
  "success_state_hint": "Marks review as approved so execution can continue.",
  "example_payload": {
    "kind": "approve_review",
    "approved": true
  },
  "resume_mode": "resume_or_fork",
  "primary_key": "approved",
  "accepted_keys": ["approved"],
  "fallback_keys": [],
  "requires_input": false,
  "supports_json_value": false,
  "args_schema": {
    "type": "object",
    "properties": {
      "approved": {
        "type": "boolean",
        "description": "Optional explicit approval flag. Defaults to true."
      }
    },
    "required": []
  }
}
```

And a text-input command looks like:

```json
{
  "name": "append_feedback",
  "label": "Append Feedback",
  "description": "Append one host feedback message to the user-visible input.",
  "category": "input",
  "input_mode": "text",
  "placeholder": "Add one short feedback message for the next run.",
  "success_state_hint": "Appends one user-visible feedback message to the canonical input.",
  "example_payload": {
    "kind": "append_feedback",
    "text": "please add tests"
  },
  "resume_mode": "resume_or_fork",
  "primary_key": "text",
  "accepted_keys": ["text", "message"],
  "fallback_keys": ["message"],
  "requires_input": true,
  "supports_json_value": false
}
```

Taken together, these two examples define the two stable host-facing extremes:
commands that require no extra input, and commands that want one text payload.
For text-heavy commands such as `append_feedback` or `override_final_output`,
hosts should prefer `placeholder` for one-line input hints,
`example_payload` for prefilled JSON/CLI examples, and `success_state_hint`
for optimistic UI copy after command application succeeds.

### Subagent tool adapters

`Runtime V2` also adds one narrow bridge from `session/app` into the existing
tool runtime and registry surfaces:

- `turbo_agent_subagent_add_tool_runtime(...)`
- `turbo_agent_subagent_add_tool_registry(...)`

This keeps composition additive:

- shared mode reuses one owned app and keeps thread/run lineage across tool calls
- stateless mode creates one fresh ephemeral app per tool call
- tool arguments may arrive as canonical `messages`, one `input` string, or any
  JSON value serialized into text

The returned tool payload is one JSON object carrying `ok`, durable ids, the
runtime `summary`, and either `output_text` or `output_json`.

For child-run-aware hosts, subagent tool payloads also expose:

- `child_thread_id`
- `child_run_id`
- `child_checkpoint_id`
- `child_status`
- nullable `parent_agent_run_id`
- nullable `parent_tool_call_id`
- nullable `parent_tool_name`

When the parent records these payloads under `tool_results.outputs[]`, the
state helpers can read child lineage directly from the recorded output item.

When a host already knows the parent run context ahead of time, `Runtime V2`
may also persist `parent_agent_run_id`, `parent_tool_call_id`, and
`parent_tool_name` onto child run/checkpoint records so those child runs can be
listed later as a durable lineage index.

Those same three fields are also echoed on the returned runtime `summary` when
the run starts from `turbo_agent_runtime_start_bind_graph_linked(...)` or from
one session/app configured with parent lineage defaults.

When one subagent run starts inside an active parent tool call, and the child
session did not set explicit parent metadata, the session now also inherits the
current runtime tool context automatically:

- parent `run_id` -> `parent_agent_run_id`
- current tool `call_id` -> `parent_tool_call_id`
- current tool name -> `parent_tool_name`

That same fallback now also applies to:

- `turbo_agent_session_list_child_runs(session, NULL, ...)`
- `turbo_agent_app_list_child_runs(app, NULL, ...)`

when those calls happen inside the active parent tool execution window.

### Preset graph builders

`Runtime V2` now also adds three high-level graph builders around the owned
agent:

- `turbo_agent_session_create_loop_graph(...)`
- `turbo_agent_session_create_review_graph(...)`
- `turbo_agent_session_create_engineering_graph(...)`

These return one ready-to-run graph with stable builtin node names.

They do not replace raw workflow installers. They remove the most repetitive
host glue for the common “one session, one agent, one canned workflow” path.

For hosts that do not want to hold a temporary graph object at all, the preset
run wrappers build the graph, run `start/resume/fork`, then destroy the graph
before returning.

For the most common one-shot path, `turbo_agent_session_start_preset_text(...)`
also creates bind-native agent state from one user message before starting.

For hosts that already keep canonical prompt messages, the parallel helpers:

- `turbo_agent_session_create_input_messages_state_bind(...)`
- `turbo_agent_session_start_preset_messages(...)`
- `turbo_agent_session_invoke_preset_messages_text(...)`
- `turbo_agent_session_invoke_preset_messages_json(...)`

remove the last host-side step of collapsing a real message array into one
fake user string.

For the next step up, `turbo_agent_session_invoke_preset_text(...)` also
extracts the final user-facing answer text so hosts no longer need to convert
bind state back into JSON just to read the result.

For structured-output flows, `turbo_agent_session_invoke_preset_json(...)`
runs the same path and then parses the final answer into one JSON tree.

When the session owns one optional long-term memory store, hosts may also use:

- `turbo_agent_session_memory_store(...)`
- `turbo_agent_session_memory_get(...)`
- `turbo_agent_session_memory_put(...)`
- `turbo_agent_session_memory_put_context(...)`
- `turbo_agent_session_memory_delete(...)`
- `turbo_agent_session_memory_list(...)`
- `turbo_agent_session_load_memory_context(...)`
- `turbo_agent_session_create_input_state_with_memory_bind(...)`
- `turbo_agent_session_create_input_messages_state_with_memory_bind(...)`
- `turbo_agent_session_invoke_preset_text_with_memory(...)`
- `turbo_agent_session_invoke_preset_json_with_memory(...)`

That keeps one session, one runtime, and one optional memory store on one host
object instead of threading extra handles through application glue.

`memory_put_context(...)` stores one formal record with:

- `scope`
- `path`
- `text`

`load_memory_context(...)` then reads matching records back into
`state.memory_context.layers` and fails loudly on malformed entries.

For the common host path, the text-first wrappers:

- `turbo_agent_session_start_preset_text_with_memory(...)`
- `turbo_agent_session_invoke_preset_text_with_memory(...)`
- `turbo_agent_session_invoke_preset_json_with_memory(...)`

let one session combine user text plus long-term memory context without making
the host manually build or reserialize state objects.

## Long-term memory store

`turbo_agent_memory_store_t` is a separate surface from:

- `turbo_agent_store_t`
- `turbo_agent_runtime_store_t`

They solve different problems:

- `turbo_agent_store_t`: explicit load/save for `state.memory`
- `turbo_agent_runtime_store_t`: durable execution lineage
- `turbo_agent_memory_store_t`: namespaced long-term memory records

When attached through `turbo_agent_session_config_t.memory_store`, the session
becomes the owner of that long-term memory store as well. The new
`memory_list_records/query_records` helpers are canonical host-facing views
layered on top of the existing raw store. They normalize each record into:

- `id`
- `namespace`
- `kind`
- `key`
- `text`
- `metadata`
- `created_at`

Legacy records currently surface `created_at = null`; formal context records
derive `kind/text/metadata` from the existing `scope/path/text` payload shape.

### Memory record shape

The stable record shape is:

- `namespace`
- `key`
- `value_json`

### Memory store API

- `turbo_agent_memory_store_memory_create(...)`
- `turbo_agent_memory_store_file_create(...)`
- `turbo_agent_memory_store_destroy(...)`
- `turbo_agent_memory_get(...)`
- `turbo_agent_memory_put(...)`
- `turbo_agent_memory_delete(...)`
- `turbo_agent_memory_list(...)`
- optional native `query(namespace_prefix, kind, key_prefix, text_substring)`
  callback on `turbo_agent_memory_store_t`

When the native `query` callback is absent, `turbo_agent_memory_list_records(...)`
and `turbo_agent_memory_query_records(...)` still fall back to the existing raw
`list(...)` callback plus canonicalization/filtering. The native `query`
callback itself returns the canonical record array shape, not the raw
`namespace/key/value_json` entries used by `list(...)`.

### Reference backends

Memory backend:

- heap-backed
- good for tests and embedding

File backend:

- writes one record per file
- layout:
  - `root_dir/records/<hex(namespace)>__<hex(key)>.json`

The file backend stores raw JSON payload text and validates it again on read.
Corrupted files fail loudly.

## Minimal usage

Session bootstrap:

```c
turbo_agent_session_config_t config = {0};
turbo_agent_session_t *session;

config.runtime_store = turbo_agent_runtime_store_file_create("./runtime-store");
config.env_path = ".env";
config.load_env = 1;

session = turbo_agent_session_create(&config);
```

Long-term memory:

```c
turbo_agent_memory_store_t memory = turbo_agent_memory_store_file_create("./memory-store");

turbo_agent_memory_put(&memory, "user/alice", "profile", "{\"name\":\"Alice\"}");
```

Canonical memory query:

```c
json_value_t *records = NULL;

turbo_agent_memory_put(
    &memory, "project/demo", "context",
    "{\"scope\":\"project\",\"path\":\"/tmp/notes.md\",\"text\":\"remember this\"}");
turbo_agent_memory_query_records(&memory, "project", "context", "con", "remember", &records);
```

The stable canonical record shape is also locked by the Stage 4 golden fixtures:

- `memory_context_record.golden.json`
- `memory_json_record.golden.json`
- `memory_query_results.golden.json`

The runtime-only cross-thread observability query contract is also locked by
`observability_queries.golden.json`.

## What Runtime V2 still does not do

It still does not add:

- high-level `invoke(...)` / `stream(...)` presets
- long-term memory search and indexing
- full subgraph persistence modes
- server, HTTP, MCP, or A2A surfaces

The new local JSON-RPC dispatcher does not change that boundary. It is only a
transport-neutral bridge over existing runtime APIs, not a hosted service.

Those remain the next layer.
