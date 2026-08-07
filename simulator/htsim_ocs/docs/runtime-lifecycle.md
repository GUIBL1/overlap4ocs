# Phase 05 OCS runtime lifecycle

The standalone runtime consumes one immutable, parser-validated
`OcsExecutionPlanV2`. It owns one HTSim `EventList`; planes share only that
clock. Every plane owns its program cursor, epoch state, physical generation,
reconfiguration timer, serializers, routes, pipes, sinks, and in-flight
counter.

## Program epoch and physical generation

The two counters are deliberately independent:

```text
program epoch 0, initial(config A)       generation 0
        |
        +-- retain(config A) ----------> program epoch 1, generation 0
        |
        +-- reconfigure(config B) -----> program epoch 2, generation 1
        |
        +-- reconfigure(config A) -----> program epoch 3, generation 2
```

An epoch follows this state machine:

```text
PENDING
  -> PATH_PREPARING
       -> RECONFIGURING (reconfigure only)
  -> PATH_READY / READY_RESERVED
  -> ACTIVE
  -> DRAINING
  -> CLOSED
  -> next program epoch, or PROGRAM_COMPLETE
```

`retain` advances the program cursor without changing generation or scheduling
a timer. `reconfigure` can start only after every group in the old epoch is
receiver-complete and that plane has no serializer backlog or in-flight transit
unit. Installation is atomic at the timer callback. Packet metadata captures
plane, program epoch, configuration, and physical generation; the switch
validates all four.

## Dependency and release lifecycle

```text
token publish
  -> decrement each declared child's remaining-parent counter once
  -> DATA_READY
  -> wait for its exact plane epoch/path and policy transfer gate
  -> group release in flow_group_id / flow_id order
  -> serializer / switch / Pipe / receiver sink
  -> flow receiver-complete exactly once
  -> group receiver-complete
  -> group token, and optionally step token, publish
```

`global_step_barrier` and `explicit_group_dag` use the same token tracker; only
the validated plan's edges differ. A group in an already prepared epoch remains
`READY_RESERVED` until its declared data parents complete. Groups in the same
epoch are not implicit parents of one another.

Path policy changes only path and transfer guards:

- `overlap_earliest`: the next plane-local path may prepare after that plane
  drains, while other planes continue their current data transfer.
- `step_lockstep`: the previous step-complete token gates path preparation and
  `lockstep_all_paths_ready` gates every transfer in the next step.
- `static_preinstalled`: every used plane has one initial epoch at generation
  zero; no retain or reconfiguration event exists.

## Coordinator order and termination

After every receiver or reconfiguration callback, the non-recursive fixed-point
pump repeatedly applies this canonical order:

```text
close completed/drained epochs
  -> prepare permitted paths
  -> publish lockstep all-paths-ready gates
  -> release permitted groups in canonical ID order
```

Success requires all explicit flows and groups receiver-complete, exact byte
conservation, all plane programs complete, and all data planes drained. Before
each dispatch, the watchdog checks the plan-owned event and simulation-time
budgets. An empty EventList before success is a deadlock. Failure summaries
retain the pre-cleanup queues/in-flight counts, unfinished IDs, remaining
parents, pending path tokens, plane states, next event time, and the complete
operation trace; cleanup then cancels timers, serializers, and Pipe-held
transit units.

The runtime emits only an in-memory `OcsRunSummary` plus operation events.
Phase 06 may serialize those objects but must not change these guards or timing
semantics.
