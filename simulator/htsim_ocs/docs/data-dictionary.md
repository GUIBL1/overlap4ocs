# HTSim OCS v2 data dictionary

The normative schemas are `simulator/schemas/swot-execution-plan-v2.schema.json`,
`swot-simulation-result-v2.schema.json`, and
`htsim-ocs-operation-event-v1.schema.json`. This document is an operational
index; the schemas and Contracts 01/04 remain authoritative.

## Units and completion

All IDs are zero-based unsigned integers. Payload is in bytes, rate in bits per
second, and simulated time in integer picoseconds. Serialization is
`ceil(payload_bytes * 8 * 10^12 / per_plane_bps)`. Data latency is one forward
Pipe latency. Successful CCT starts at zero and ends at the receiver's last
mandatory payload byte.

## Plan v2

| Tree | Meaning |
|---|---|
| `topology` | rank/plane counts, per-plane endpoint rate, data latency, reconfiguration delay, duplex, initial-install policy |
| `transport` | `paper_exact`, MTU/exact tail, lossless payload-only wire, no ACK, receiver completion |
| `execution_mode` | `full_packet` or proven-equivalent `exact_coalesced` |
| `configurations[]` | complete rank permutations; they never imply traffic |
| `flows[]` | explicit positive-byte source/destination transfers and segment slices |
| `flow_groups[]` | atomic completion group, step/plane/config/program epoch, dependency tokens, explicit flow membership |
| `steps[]` | logical step membership and completion token |
| `readiness_tokens[]` | collective-start, group-complete, and step-complete typed tokens |
| `plane_programs[]` | ordered initial/retain/reconfigure epochs and path-preparation guards |
| `run_limits` | sole seed, simulated-time limit, and event limit |
| `planner_certificate`, `provenance`, `workload` | auditable input identity; the backend does not reinterpret them |

`plan_file_sha256` is SHA-256 of the exact UTF-8 file bytes. It is not a plan
field and is never calculated from reserialized JSON.

## Result v2

| Tree | Meaning |
|---|---|
| `status`, `stop_reason`, `error` | closed outcome taxonomy and stable error identity |
| `timing` | collective start/complete, CCT, and actual simulation stop; failure CCT/complete are null |
| mode/identity fields | exact echo of trusted plan identity, or null for untrusted raw/schema failures |
| `tokens[]` | pending/ready state and receiver-complete-derived ready time |
| `steps[]` | expected/completed group counts, first release, receiver completion |
| `flow_groups[]` | dependency/path release, epoch/generation identity, byte/count aggregates, completion |
| `flows[]` | source/destination, logical packets, exact sent/received bytes and last-byte times |
| `planes[].epochs[]` | transition, path-prep/ready, transfer/drain/close, reconfiguration, program epoch and physical generation |
| `planes[].source_ports[]` | sent bytes, logical packets, backlog, busy intervals and stop truncation |
| `traffic` | global, per-rank, and per-plane conservation plus event/transit counters |
| `blocked_state` | unfinished entities, next event, and per-plane cursor/state on runtime failure |
| `invariants` | the twelve mandatory dependency/route/epoch/generation/byte/aggregate guards |
| `provenance` | binary, compiler, build flags, upstream/patchset, schema, and coalescing-proof identity |

Arrays use canonical ID order. JSON objects are key-sorted and encoded as one
UTF-8 line with a final newline. Busy intervals are half-open durations
`[start_ps,end_ps]` for accounting; `truncated_at_stop=true` identifies an
active interval snapped at failure stop.

## Operation JSONL

Every line is a complete `htsim-ocs-operation-event/v1` object. Event index is
contiguous and time is nondecreasing. The fixed fields are schema/event/time,
step/group/flow/token/plane/program epoch/configuration/physical generation,
and reason. Non-applicable fields are explicit nulls. In particular,
`path_prep_start` has no physical generation because cutover has not occurred.

