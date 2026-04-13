# CoroNet Benchmark Matrix

This matrix exists to stop benchmark discussions from drifting into anecdotes.
Every optimization pass should update the same shape of data.

## What To Measure

### 1. Local Datapath Latency

Source:
- `build/Msvc-Release/bin/test_bench_coro.exe`

Filters:
- `ws_echo_roundtrip`
- `wss_echo_roundtrip`
- `ws_connect_breakdown`
- `wss_connect_breakdown`
- `wss_echo_roundtrip_tls13`
- `wss_connect_breakdown_tls13`

Purpose:
- Separate handshake cost from steady-state payload cost.
- Track `WSS over WS` ratio, not just absolute numbers.

Primary success metrics:
- `ws_echo_single_exchange_pooled`
- `ws_echo_1k_16exchanges_pooled`
- `wss_echo_single_exchange_pooled`
- `wss_echo_1k_16exchanges_pooled`
- `wss_tls_resumed_connect_hot`
- `wss_upgrade_hot`

### 2. Connection Scale / RSS

Purpose:
- Measure whether CoroNet is competitive on connection density, not just microbench latency.

Required output:
- target connections
- successful connections
- process RSS / working set
- per-connection memory estimate
- idle CPU during hold phase

Do not claim parity with `uWebSockets` until this exists.

Suggested command:

```powershell
pwsh -File tools/benchmarks/measure_ws_scale.ps1 `
  -Binary build/Msvc-Release/bin/test_ws_scale_bench.exe `
  -Transports ws,wss `
  -Connections 1000,5000 `
  -OutJson tmp/ws-scale.json `
  -OutMarkdown tmp/ws-scale.md
```

### 3. Standards Compliance

Tool:
- Autobahn Testsuite

Required output:
- case count
- pass / fail summary
- failing case ids
- generated Autobahn report path

Do not write `100% compliant` without the report.

## Recommended Workflow

### Collect local latency matrix

```powershell
pwsh -File tools/benchmarks/collect_ws_matrix.ps1 `
  -Binary build/Msvc-Release/bin/test_bench_coro.exe `
  -OutJson tmp/ws-matrix.json `
  -OutMarkdown tmp/ws-matrix.md `
  -BaselineJson tools/benchmarks/uwebsockets.baseline.sample.json
```

### Review the key question

Ask only this first:

`How much slower is WSS than WS on the same machine, same payload, same binary?`

If that ratio does not improve, the optimization probably did not hit the real bottleneck.

## Interpretation Rules

- `hot` numbers are mostly lifecycle and handshake.
- `persistent` and `pooled` numbers are the real steady-state datapath.
- Small-message wins do not prove large-payload wins.
- A single benchmark run is not enough for claims about regressions under 5%.
- Never compare CoroNet localhost microbenchmarks to external `uWS` connection-count claims as if they were the same test.

## External Baselines

When you compare against `uWebSockets`, keep these fields separate:

- vendor claim
- external third-party benchmark
- your own local measurement

If those three are mixed into one table, the result is garbage.
