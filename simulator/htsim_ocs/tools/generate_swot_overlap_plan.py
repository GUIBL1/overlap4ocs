"""Generate a 3-node / 3-plane SWOT overlap execution plan for testing.

Emits a ``swot`` / ``overlap_earliest`` plan (the paper Fig.5 pattern): a
forward-ring reduce-scatter followed by a reversed-ring allgather, so the
planes must reconfigure between the phases.  The per-step byte assignment
is deliberately uneven across planes (planner choice, as in the
paper_fig5_swot fixture): the plane carrying the heavy step-1 chunk keeps
transmitting while the other two planes drain, reconfigure, and are
path-ready long before the step-1 barrier.  All three reconfigurations
therefore hide inside another plane's transmission window, which is the
point of the test case.

Concrete story (times in ps, 400 Gb/s, latency 20, reconfiguration 2000):

  - Step 0 (reduce_scatter, cfg0 [1,2,0]): 1000 B on each plane, all done at 20020.
  - Step 1 (reduce_scatter, cfg0): plane 0 carries 1000+1000 B (done 40040),
    plane 1 carries 4000 B (done 100040); plane 2 is idle.
    Plane 2 reconfigures 20020->22020, plane 0 reconfigures 40040->42040,
    both hidden under plane 1's 4000 B transmission.
  - Step 2 (allgather, cfg1 [2,0,1]): planes 0 and 2 carry 2000 B each
    (released at the step-1 barrier 100040, paths already ready).
  - Step 3 (allgather, cfg1): 1000 B on planes 0 and 2.  Plane 1 needs no
    reconfiguration (no cfg1 flows) and its program ends at 100040.
    CCT = 160080.

Under step_lockstep the same flows cost 162080, so the case shows a
2000 ps gain from hidden reconfiguration on the critical path.

Run from the repository root with the ``ocs`` conda environment:

    PYTHONPATH=. python simulator/htsim_ocs/tools/generate_swot_overlap_plan.py
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
from typing import Any

from simulator.contracts.io import file_sha256, write_canonical_json
from simulator.contracts.validation import validate_execution_plan

REPO_ROOT = Path(__file__).resolve().parents[3]

# (flow_id, group_id, src_rank, dst_rank, payload_bytes, chunk_index)
# chunk_index marks which 1000-byte slot of the rank's logical message this
# flow carries (identity/audit only; the backend does not verify bytes).
FLOW_SPECS = [
    # Step 0: reduce-scatter, forward ring cfg0 [1,2,0], 1000 B each edge.
    (0, 0, 0, 1, 1000, 0),   # rank 0 -> 1, chunk 0
    (1, 1, 1, 2, 1000, 1),   # rank 1 -> 2, chunk 1
    (2, 2, 2, 0, 1000, 2),   # rank 2 -> 0, chunk 2
    # Step 1: reduce-scatter, cfg0; heavy chunk on plane 1 keeps it busy.
    (3, 3, 1, 2, 1000, 0),   # rank 1 -> 2, chunk 0
    (4, 3, 2, 0, 1000, 1),   # rank 2 -> 0, chunk 1
    (5, 4, 0, 1, 4000, 2),   # rank 0 -> 1, chunk 2 (4000 B planner choice)
    # Step 2: allgather, reversed ring cfg1 [2,0,1], 2000 B each edge.
    (6, 5, 0, 2, 2000, 0),   # rank 0 -> 2, chunk 0
    (7, 5, 2, 1, 2000, 2),   # rank 2 -> 1, chunk 2
    (8, 6, 1, 0, 2000, 1),   # rank 1 -> 0, chunk 1
    # Step 3: allgather, cfg1, 1000 B each edge.
    (9, 7, 0, 2, 1000, 1),   # rank 0 -> 2, chunk 1
    (10, 7, 2, 1, 1000, 0),  # rank 2 -> 1, chunk 0
    (11, 8, 1, 0, 1000, 2),  # rank 1 -> 0, chunk 2
]

# (group_id, step_id, plane_id, configuration_id, [flow_ids])
GROUP_SPECS = [
    (0, 0, 0, 0, [0]),
    (1, 0, 1, 0, [1]),
    (2, 0, 2, 0, [2]),
    (3, 1, 0, 0, [3, 4]),
    (4, 1, 1, 0, [5]),
    (5, 2, 0, 1, [6, 7]),
    (6, 2, 2, 1, [8]),
    (7, 3, 0, 1, [9, 10]),
    (8, 3, 2, 1, [11]),
]

# (plane_id, [(epoch_id, transition, configuration_id, [group_ids])])
PROGRAM_SPECS = [
    (0, [(0, "initial", 0, [0, 3]), (1, "reconfigure", 1, [5]), (2, "retain", 1, [7])]),
    (1, [(0, "initial", 0, [1, 4])]),
    (2, [(0, "initial", 0, [2]), (1, "reconfigure", 1, [6, 8])]),
]

STEP_SPECS = [
    (0, "reduce_scatter_0", [0, 1, 2]),
    (1, "reduce_scatter_1", [3, 4]),
    (2, "allgather_0", [5, 6]),
    (3, "allgather_1", [7, 8]),
]


def build_plan(
    *,
    name: str,
    mtu_bytes: int,
    per_plane_bps: int,
    data_latency_ps: int,
    reconfiguration_delay_ps: int,
    execution_mode: str,
) -> dict[str, Any]:
    flows = []
    segments = []
    for flow_id, group_id, src, dst, payload, chunk in FLOW_SPECS:
        flows.append(
            {
                "dst_rank": dst,
                "flow_group_id": group_id,
                "flow_id": flow_id,
                "payload_bytes": payload,
                "segment_slices": [
                    {
                        "length_bytes": payload,
                        "segment_id": flow_id,
                        "segment_offset_bytes": 0,
                    }
                ],
                "src_rank": src,
            }
        )
        segments.append(
            {
                "buffer_offset_bytes": chunk * 1000,
                "final_destination_rank_ids": [dst],
                "length_bytes": payload,
                "origin_rank_ids": [src],
                "segment_id": flow_id,
            }
        )
    # Tokens: 0 = collective_start; per step: one token per group, then one
    # step token.  Groups are ordered by step, so group_token is indexed by
    # group_id once groups are sorted.
    group_specs = sorted(GROUP_SPECS)
    tokens: list[dict[str, Any]] = [
        {"producer_id": None, "token_id": 0, "token_type": "collective_start"}
    ]
    next_token = 1
    group_token: dict[int, int] = {}
    step_token: dict[int, int] = {}
    for step_id, _, group_ids in STEP_SPECS:
        for group_id in group_ids:
            group_token[group_id] = next_token
            tokens.append(
                {
                    "producer_id": group_id,
                    "token_id": next_token,
                    "token_type": "flow_group_complete",
                }
            )
            next_token += 1
        step_token[step_id] = next_token
        tokens.append(
            {
                "producer_id": step_id,
                "token_id": next_token,
                "token_type": "step_complete",
            }
        )
        next_token += 1

    flow_groups = []
    for group_id, step_id, plane_id, configuration_id, flow_ids in group_specs:
        dependencies = [0] if step_id == 0 else [step_token[step_id - 1]]
        flow_groups.append(
            {
                "completion_token_id": group_token[group_id],
                "configuration_id": configuration_id,
                "depends_on_token_ids": dependencies,
                "flow_group_id": group_id,
                "flow_ids": flow_ids,
                "plane_id": plane_id,
                "program_epoch_id": None,  # filled from program specs below
                "step_id": step_id,
            }
        )

    plane_programs = []
    for plane_id, epochs in PROGRAM_SPECS:
        built_epochs = []
        for epoch_id, transition, configuration_id, group_ids in epochs:
            for group_id in group_ids:
                flow_groups[group_id]["program_epoch_id"] = epoch_id
            built_epochs.append(
                {
                    "configuration_id": configuration_id,
                    "flow_group_ids": group_ids,
                    "path_prep_not_before_token_ids": [],
                    "program_epoch_id": epoch_id,
                    "transition": transition,
                }
            )
        plane_programs.append({"epochs": built_epochs, "plane_id": plane_id})

    plan = {
        "schema_version": "swot-execution-plan/v2",
        "case_id": "case-" + hashlib.sha256(f"overlap4ocs-ring-allreduce-swot:{name}".encode()).hexdigest(),
        "strategy": "swot",
        "units": {"data": "byte", "rate": "bit_per_second", "time": "picosecond"},
        "workload": {
            "collective_id": "allreduce",
            "algorithm_id": "ring_allreduce_swot",
            "algorithm_semantics_version": "ring-swot/v1",
            "collective_semantics_version": "allreduce/v1",
            "rank_count": 3,
            "message_bytes_per_rank": 3000,
        },
        "topology": {
            "node_count": 3,
            "plane_count": 3,
            "per_plane_bps": per_plane_bps,
            "data_latency_ps": data_latency_ps,
            "reconfiguration_delay_ps": reconfiguration_delay_ps,
            "duplex": "full",
            "initial_configuration_policy": "first_use_preinstalled",
        },
        "transport": {
            "mode": "paper_exact",
            "wire_model": "payload_only",
            "packetization": "exact_tail",
            "mtu_bytes": mtu_bytes,
            "loss_mode": "lossless",
            "ack_mode": "none",
            "completion_semantics": "receiver_last_payload_byte",
        },
        "execution_mode": execution_mode,
        "run_limits": {
            "seed": 0,
            "max_sim_time_ps": 1000000000000000,
            "max_events": 10000000,
        },
        "dependency_mode": "global_step_barrier",
        "path_preparation_policy": "overlap_earliest",
        "configurations": [
            {"configuration_id": 0, "permutation": [1, 2, 0]},
            {"configuration_id": 1, "permutation": [2, 0, 1]},
        ],
        "logical_segments": segments,
        "readiness_tokens": tokens,
        "steps": [
            {
                "step_id": step_id,
                "phase": phase,
                "flow_group_ids": group_ids,
                "completion_token_id": step_token[step_id],
            }
            for step_id, phase, group_ids in STEP_SPECS
        ],
        "flow_groups": flow_groups,
        "flows": flows,
        "plane_programs": plane_programs,
        "planner_certificate": {
            "planner_name": "hand_authored_fixture",
            "planner_version": "v1",
            "solver_name": None,
            "solver_status": "not_used",
            "objective_ps": None,
            "bound_ps": None,
            "relative_gap_ppm": None,
            "decision_sha256": None,
            "nominal_schedule": [],
            "integer_lowering_rule": "hand_authored_exact_bytes",
            "integer_lowering_version": "v1",
        },
        "provenance": {
            "source_kind": "hand_authored_fixture",
            "overlap4ocs_git_sha": None,
            "htsim_upstream_git_sha": None,
            "htsim_local_patchset_sha256": None,
            "instance_file_sha256": None,
            "program_file_sha256": None,
            "collective_ir_sha256": None,
            "plan_build_context_sha256": None,
        },
    }
    validate_execution_plan(plan)
    return plan


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mtu", type=int, default=1500, help="MTU bytes (default 1500)")
    parser.add_argument("--per-plane-bps", type=int, default=400000000000, help="per-plane rate in bps (default 400 Gb/s)")
    parser.add_argument("--data-latency-ps", type=int, default=20, help="one-way latency in ps (default 20)")
    parser.add_argument("--reconfiguration-delay-ps", type=int, default=2000, help="reconfiguration delay in ps (default 2000)")
    parser.add_argument("--output-dir", type=Path, default=None, help="output directory (default simulator/htsim_ocs/tests/full_simulator_test/ring_allreduce_swot_p3k3)")
    args = parser.parse_args()

    output_dir = args.output_dir or (
        REPO_ROOT
        / "simulator"
        / "htsim_ocs"
        / "tests"
        / "full_simulator_test"
        / "ring_allreduce_swot_p3k3"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    for mode in ("full_packet", "exact_coalesced"):
        plan = build_plan(
            name="p3k3",
            mtu_bytes=args.mtu,
            per_plane_bps=args.per_plane_bps,
            data_latency_ps=args.data_latency_ps,
            reconfiguration_delay_ps=args.reconfiguration_delay_ps,
            execution_mode=mode,
        )
        path = output_dir / f"plan-{mode.replace('_', '-')}.json"
        write_canonical_json(path, plan)
        print(f"wrote {path} sha256={file_sha256(path)}")


if __name__ == "__main__":
    main()
