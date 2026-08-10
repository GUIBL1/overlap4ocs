"""Generate ring-allreduce execution plans (swot-execution-plan/v2) for testing.

Emits a one-shot static-preinstalled plan for ``p`` ranks on ``k`` parallel
OCS planes: every plane is preinstalled with the forward ring permutation
(0->1, 1->2, ..., p-1->0) and the 2*(p-1) ring-allreduce steps (p-1
reduce-scatter followed by p-1 allgather) run one after another under the
global step barrier, with each step's p ring edges spread across the k
planes in rotation.  Per rank the message is split into p equal chunks, so
message_bytes_per_rank must be divisible by p; each flow carries exactly
one chunk.

The generator writes a strict-JSON plan per execution mode
(plan-full-packet.json and plan-exact-coalesced.json, differing only in
``execution_mode``), validates both with the repo semantic validator, and
prints their canonical SHA-256 digests.

Usage (from the repository root so PYTHONPATH resolves the contracts):

    PYTHONPATH=. python simulator/htsim_ocs/tools/generate_ring_allreduce_plan.py
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
from typing import Any

from simulator.contracts.io import file_sha256, write_canonical_json
from simulator.contracts.validation import validate_execution_plan

REPO_ROOT = Path(__file__).resolve().parents[3]


def build_plan(
    *,
    name: str,
    p: int,
    planes: int,
    message_bytes_per_rank: int,
    mtu_bytes: int,
    per_plane_bps: int,
    data_latency_ps: int,
    reconfiguration_delay_ps: int,
    execution_mode: str,
) -> dict[str, Any]:
    if message_bytes_per_rank % p != 0:
        raise ValueError("message_bytes_per_rank must be divisible by p")
    chunk_bytes = message_bytes_per_rank // p
    steps_total = 2 * (p - 1)

    # Ring allreduce: at step s every rank i sends chunk (i - s) mod p to
    # rank (i + 1) mod p.  The first p-1 steps are reduce-scatter, the
    # remaining p-1 are allgather; in both phases the step index in the
    # chunk formula is the phase-local index (0..p-2), so the allgather
    # phase restarts from its own step zero.
    edges = [
        (
            rank,
            (rank + 1) % p,
            (rank - (step if step < p - 1 else step - (p - 1))) % p,
        )
        for step in range(steps_total)
        for rank in range(p)
    ]

    # Tokens: 0 = collective_start; then per step p group tokens + 1 step
    # token.  Group ids and flow ids are numbered by (step, edge index).
    tokens: list[dict[str, Any]] = [
        {"producer_id": None, "token_id": 0, "token_type": "collective_start"}
    ]
    group_token: list[int] = []
    step_token: list[int] = []
    next_token = 1
    for step in range(steps_total):
        for _ in range(p):
            group_token.append(next_token)
            tokens.append(
                {
                    "producer_id": len(group_token) - 1,
                    "token_id": next_token,
                    "token_type": "flow_group_complete",
                }
            )
            next_token += 1
        step_token.append(next_token)
        tokens.append(
            {
                "producer_id": step,
                "token_id": next_token,
                "token_type": "step_complete",
            }
        )
        next_token += 1

    flow_groups: list[dict[str, Any]] = []
    flows: list[dict[str, Any]] = []
    segments: list[dict[str, Any]] = []
    for group_id, (src_rank, dst_rank, chunk) in enumerate(edges):
        step = group_id // p
        edge_index = group_id % p
        dependencies = [0] if step == 0 else [step_token[step - 1]]
        plane_id = (edge_index + step) % planes
        flow_groups.append(
            {
                "completion_token_id": group_token[group_id],
                "configuration_id": 0,
                "depends_on_token_ids": dependencies,
                "flow_group_id": group_id,
                "flow_ids": [group_id],
                "plane_id": plane_id,
                "program_epoch_id": 0,
                "step_id": step,
            }
        )
        flows.append(
            {
                "dst_rank": dst_rank,
                "flow_group_id": group_id,
                "flow_id": group_id,
                "payload_bytes": chunk_bytes,
                "segment_slices": [
                    {
                        "length_bytes": chunk_bytes,
                        "segment_id": group_id,
                        "segment_offset_bytes": 0,
                    }
                ],
                "src_rank": src_rank,
            }
        )
        segments.append(
            {
                "buffer_offset_bytes": chunk * chunk_bytes,
                "final_destination_rank_ids": [dst_rank],
                "length_bytes": chunk_bytes,
                "origin_rank_ids": [src_rank],
                "segment_id": group_id,
            }
        )

    groups_by_plane: list[list[int]] = [[] for _ in range(planes)]
    for group in flow_groups:
        groups_by_plane[group["plane_id"]].append(group["flow_group_id"])

    phases = [
        f"reduce_scatter_{step}" if step < p - 1 else f"allgather_{step - (p - 1)}"
        for step in range(steps_total)
    ]
    plan = {
        "schema_version": "swot-execution-plan/v2",
        "case_id": "case-" + hashlib.sha256(f"overlap4ocs-ring-allreduce:{name}".encode()).hexdigest(),
        "strategy": "one_shot",
        "units": {"data": "byte", "rate": "bit_per_second", "time": "picosecond"},
        "workload": {
            "collective_id": "allreduce",
            "algorithm_id": "ring_allreduce",
            "algorithm_semantics_version": "ring/v1",
            "collective_semantics_version": "allreduce/v1",
            "rank_count": p,
            "message_bytes_per_rank": message_bytes_per_rank,
        },
        "topology": {
            "node_count": p,
            "plane_count": planes,
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
        "path_preparation_policy": "static_preinstalled",
        "configurations": [
            {
                "configuration_id": 0,
                "permutation": [(rank + 1) % p for rank in range(p)],
            }
        ],
        "logical_segments": segments,
        "readiness_tokens": tokens,
        "steps": [
            {
                "step_id": step,
                "phase": phases[step],
                "flow_group_ids": list(range(step * p, (step + 1) * p)),
                "completion_token_id": step_token[step],
            }
            for step in range(steps_total)
        ],
        "flow_groups": flow_groups,
        "flows": flows,
        "plane_programs": [
            {
                "plane_id": plane_id,
                "epochs": [
                    {
                        "program_epoch_id": 0,
                        "transition": "initial",
                        "configuration_id": 0,
                        "path_prep_not_before_token_ids": [],
                        "flow_group_ids": sorted(groups_by_plane[plane_id]),
                    }
                ],
            }
            for plane_id in range(planes)
        ],
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
    parser.add_argument("--p", type=int, default=3, help="rank count (default 3)")
    parser.add_argument("--planes", type=int, default=3, help="plane count (default 3)")
    parser.add_argument("--message-bytes-per-rank", type=int, default=3000, help="message size per rank (default 3000)")
    parser.add_argument("--mtu", type=int, default=1500, help="MTU bytes (default 1500)")
    parser.add_argument("--per-plane-bps", type=int, default=400000000000, help="per-plane rate in bps (default 400 Gb/s)")
    parser.add_argument("--data-latency-ps", type=int, default=20, help="one-way latency in ps (default 20)")
    parser.add_argument("--reconfiguration-delay-ps", type=int, default=200, help="reconfiguration delay in ps (default 200)")
    parser.add_argument("--output-dir", type=Path, default=None, help="output directory (default simulator/htsim_ocs/tests/full_simulator_test/ring_allreduce_p<k>k)")
    args = parser.parse_args()

    name = f"p{args.p}k{args.planes}m{args.message_bytes_per_rank}"
    output_dir = args.output_dir or (
        REPO_ROOT
        / "simulator"
        / "htsim_ocs"
        / "tests"
        / "full_simulator_test"
        / f"ring_allreduce_{name}"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    for mode in ("full_packet", "exact_coalesced"):
        plan = build_plan(
            name=name,
            p=args.p,
            planes=args.planes,
            message_bytes_per_rank=args.message_bytes_per_rank,
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
