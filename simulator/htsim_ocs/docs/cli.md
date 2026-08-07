# `htsim_ocs` standalone CLI

`htsim_ocs` is a file-only simulator backend. Its only formal run interface is:

```bash
build/htsim_ocs run --plan INPUT.json --result OUTPUT.json \
  [--trace OPERATIONS.jsonl]
```

The plan fixes transport, execution, dependency, path-preparation, seed,
maximum simulated time, and maximum event count. The CLI has no overrides and
does not read stdin, TOML, Python objects, solver state, or environment case
parameters.

## Commands

```text
run --plan PATH --result PATH [--trace PATH]
validate --plan PATH
capabilities --json
version
help
```

`validate` prints one human-readable summary after strict Plan v2 validation.
`run` prints only a short commit acknowledgement on success. Status, CCT, and
counters always come from `result.json`. `capabilities --json` is the sole
machine-readable stdout command. Diagnostics go to stderr.

All run paths must be distinct. The input must be a readable regular file, the
output parents must already exist and be writable, and formal output paths
must not exist. Existing results and traces are never overwritten.

## Exit classes

| Code | Meaning | Formal stop reasons |
|---:|---|---|
| 0 | success | `collective_complete` |
| 2 | invalid or unsupported | `invalid_input`, `unsupported_semantics`, invalid CLI/preflight |
| 3 | runtime invariant/deadlock | `deadlock`, `invariant_violation` |
| 4 | plan simulation limit | `max_simulation_time`, `max_event_count` |
| 5 | I/O or internal failure | `io_error`, `internal_error` |

When raw plan bytes are readable, validation and known runtime failures write a
Result v2 whenever the result path can be committed. A failure never contains
a CCT. An unreadable plan, invalid output preflight, or failed result commit may
leave no formal result; the exit code and stderr are then the only invocation
diagnostic, not a simulated outcome.

## Atomic commit protocol

Each output is written to a unique same-directory temporary file, flushed with
`fsync`, closed, and installed with no-replace atomic rename; the directory is
then flushed. If trace is requested it is committed first. The final result
rename is the invocation commit point. A trace failure produces an `io_error`
result and never publishes the computed success result. Temporary filenames,
PID, host, wall clock, and output paths do not enter canonical data.

## Reproducible invocation

```bash
source "$(conda info --base)/etc/profile.d/conda.sh"
conda activate ocs
make -C simulator/htsim_ocs clean all

run_dir="$(mktemp -d)"
simulator/htsim_ocs/build/htsim_ocs run \
  --plan simulator/htsim_ocs/tests/fixtures/v2/runtime/paper_fig5_swot/plan-full-packet.json \
  --result "$run_dir/result.json" \
  --trace "$run_dir/operations.jsonl"
```

