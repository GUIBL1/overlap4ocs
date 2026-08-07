"""Author the hand-specified Phase 02 cross-language fixture corpus."""

from __future__ import annotations

import copy
import hashlib
import math
from collections import defaultdict
from pathlib import Path
from typing import Any

from simulator.contracts.constants import MANDATORY_INVARIANTS, SCHEMA_FILES
from simulator.contracts.io import canonical_json_bytes, file_sha256, read_json_file
from simulator.contracts.validation import (
    validate_capabilities_projection,
    validate_contract_pair,
    validate_execution_plan,
    validate_simulation_result,
)


ROOT = Path(__file__).resolve().parent
REPO_ROOT = Path(__file__).resolve().parents[4]
PLAN_DIR = ROOT / "plans" / "valid"
INVALID_PLAN_DIR = ROOT / "plans" / "invalid"
SUCCESS_DIR = ROOT / "results" / "success"
FAILURE_DIR = ROOT / "results" / "failure"
INVALID_RESULT_DIR = ROOT / "results" / "invalid"
PAIR_DIR = ROOT / "pairs"
CAPABILITY_DIR = ROOT / "capabilities"

PATCHSET_SHA = file_sha256(
    REPO_ROOT / "third_party" / "csg-htsim" / "LOCAL_PATCHSET.json"
)
PARENT_COMMIT = "beb976756c5541554edca8eed1376900f581fd18"
UPSTREAM_COMMIT = "841d9e7be46bb968eece766aa4b6c044c7799f67"
FIXTURE_BUILD_FLAGS_SHA = hashlib.sha256(
    b"hand-authored-fixture-build-flags:none\n"
).hexdigest()


def write(path: Path, document: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical_json_bytes(document))


def case_id(name: str) -> str:
    digest = hashlib.sha256(f"overlap4ocs-fixture:{name}".encode()).hexdigest()
    return f"case-{digest}"


def make_plan(
    name: str,
    *,
    strategy: str,
    dependency_mode: str,
    node_count: int,
    plane_count: int,
    configurations: list[list[int]],
    group_specs: list[dict[str, int | list[int]]],
    program_specs: list[list[dict[str, Any]]],
    mtu_bytes: int = 1500,
) -> dict[str, Any]:
    groups_by_step: dict[int, list[int]] = defaultdict(list)
    for group_id, spec in enumerate(group_specs):
        groups_by_step[int(spec["step_id"])].append(group_id)
    if sorted(groups_by_step) != list(range(len(groups_by_step))):
        raise ValueError("fixture step IDs must be contiguous")

    tokens = [
        {"producer_id": None, "token_id": 0, "token_type": "collective_start"}
    ]
    group_token: dict[int, int] = {}
    step_token: dict[int, int] = {}
    next_token = 1
    for step_id in range(len(groups_by_step)):
        for group_id in groups_by_step[step_id]:
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
    flows = []
    segments = []
    for group_id, spec in enumerate(group_specs):
        step_id = int(spec["step_id"])
        if dependency_mode == "global_step_barrier":
            dependencies = [0] if step_id == 0 else [step_token[step_id - 1]]
        else:
            parent_groups = list(spec.get("parent_groups", []))
            dependencies = (
                sorted(group_token[int(parent)] for parent in parent_groups)
                if parent_groups
                else [0]
            )
        payload = int(spec["payload_bytes"])
        flow_groups.append(
            {
                "completion_token_id": group_token[group_id],
                "configuration_id": int(spec["configuration_id"]),
                "depends_on_token_ids": dependencies,
                "flow_group_id": group_id,
                "flow_ids": [group_id],
                "plane_id": int(spec["plane_id"]),
                "program_epoch_id": int(spec["program_epoch_id"]),
                "step_id": step_id,
            }
        )
        flows.append(
            {
                "dst_rank": int(spec["dst_rank"]),
                "flow_group_id": group_id,
                "flow_id": group_id,
                "payload_bytes": payload,
                "segment_slices": [
                    {
                        "length_bytes": payload,
                        "segment_id": group_id,
                        "segment_offset_bytes": 0,
                    }
                ],
                "src_rank": int(spec["src_rank"]),
            }
        )
        segments.append(
            {
                "buffer_offset_bytes": 0,
                "final_destination_rank_ids": [int(spec["dst_rank"])],
                "length_bytes": payload,
                "origin_rank_ids": [int(spec["src_rank"])],
                "segment_id": group_id,
            }
        )

    plane_programs = []
    for plane_id, epochs in enumerate(program_specs):
        built_epochs = []
        for epoch_id, epoch in enumerate(epochs):
            group_ids = list(epoch["flow_group_ids"])
            path_gate = epoch.get("path_gate", [])
            if path_gate == "previous_step":
                step_ids = {flow_groups[group_id]["step_id"] for group_id in group_ids}
                if len(step_ids) != 1:
                    raise ValueError("previous-step gate needs one step")
                step_id = next(iter(step_ids))
                path_tokens = [] if step_id == 0 else [step_token[step_id - 1]]
            elif path_gate == "current_group":
                path_tokens = [group_token[group_ids[0]]]
            else:
                path_tokens = list(path_gate)
            built_epochs.append(
                {
                    "configuration_id": int(epoch["configuration_id"]),
                    "flow_group_ids": group_ids,
                    "path_prep_not_before_token_ids": path_tokens,
                    "program_epoch_id": epoch_id,
                    "transition": epoch["transition"],
                }
            )
        plane_programs.append({"epochs": built_epochs, "plane_id": plane_id})

    policy = {
        "baseline": "step_lockstep",
        "one_shot": "static_preinstalled",
        "swot": "overlap_earliest",
    }[strategy]
    maximum_payload = max(flow["payload_bytes"] for flow in flows)
    plan = {
        "case_id": case_id(name),
        "configurations": [
            {"configuration_id": index, "permutation": permutation}
            for index, permutation in enumerate(configurations)
        ],
        "dependency_mode": dependency_mode,
        "execution_mode": "full_packet",
        "flow_groups": flow_groups,
        "flows": flows,
        "logical_segments": segments,
        "path_preparation_policy": policy,
        "plane_programs": plane_programs,
        "planner_certificate": {
            "bound_ps": None,
            "decision_sha256": None,
            "integer_lowering_rule": "hand_authored_exact_bytes",
            "integer_lowering_version": "v1",
            "nominal_schedule": [],
            "objective_ps": None,
            "planner_name": "hand_authored_fixture",
            "planner_version": "v1",
            "relative_gap_ppm": None,
            "solver_name": None,
            "solver_status": "not_used",
        },
        "provenance": {
            "collective_ir_sha256": None,
            "htsim_local_patchset_sha256": None,
            "htsim_upstream_git_sha": None,
            "instance_file_sha256": None,
            "overlap4ocs_git_sha": None,
            "plan_build_context_sha256": None,
            "program_file_sha256": None,
            "source_kind": "hand_authored_fixture",
        },
        "readiness_tokens": tokens,
        "run_limits": {
            "max_events": 10000000,
            "max_sim_time_ps": 1000000000000000,
            "seed": 0,
        },
        "schema_version": "swot-execution-plan/v2",
        "steps": [
            {
                "completion_token_id": step_token[step_id],
                "flow_group_ids": groups_by_step[step_id],
                "phase": f"fixture_phase_{step_id}",
                "step_id": step_id,
            }
            for step_id in range(len(groups_by_step))
        ],
        "strategy": strategy,
        "topology": {
            "data_latency_ps": 20,
            "duplex": "full",
            "initial_configuration_policy": "first_use_preinstalled",
            "node_count": node_count,
            "per_plane_bps": 400000000000,
            "plane_count": plane_count,
            "reconfiguration_delay_ps": 200,
        },
        "transport": {
            "ack_mode": "none",
            "completion_semantics": "receiver_last_payload_byte",
            "loss_mode": "lossless",
            "mode": "paper_exact",
            "mtu_bytes": mtu_bytes,
            "packetization": "exact_tail",
            "wire_model": "payload_only",
        },
        "units": {
            "data": "byte",
            "rate": "bit_per_second",
            "time": "picosecond",
        },
        "workload": {
            "algorithm_id": name,
            "algorithm_semantics_version": "fixture/v1",
            "collective_id": "allreduce",
            "collective_semantics_version": "allreduce/v1",
            "message_bytes_per_rank": maximum_payload,
            "rank_count": node_count,
        },
    }
    validate_execution_plan(plan)
    return plan


def interval_union_length(intervals: list[tuple[int, int]]) -> int:
    if not intervals:
        return 0
    intervals = sorted(intervals)
    start, end = intervals[0]
    total = 0
    for next_start, next_end in intervals[1:]:
        if next_start <= end:
            end = max(end, next_end)
        else:
            total += end - start
            start, end = next_start, next_end
    return total + end - start


def result_provenance() -> dict[str, Any]:
    return {
        "build_flags_sha256": FIXTURE_BUILD_FLAGS_SHA,
        "compiler_id": "hand-authored-fixture",
        "exact_coalesced_equivalence_version": None,
        "htsim_local_patchset_sha256": PATCHSET_SHA,
        "htsim_ocs_version": "phase02-golden",
        "htsim_upstream_git_sha": UPSTREAM_COMMIT,
        "parent_build_commit": PARENT_COMMIT,
        "plan_schema_id": "swot-execution-plan/v2",
        "plan_schema_sha256": file_sha256(
            SCHEMA_FILES["swot-execution-plan/v2"]
        ),
        "result_schema_id": "swot-simulation-result/v2",
        "result_schema_sha256": file_sha256(
            SCHEMA_FILES["swot-simulation-result/v2"]
        ),
    }


def make_success_result(plan: dict[str, Any], plan_sha: str) -> dict[str, Any]:
    mtu = plan["transport"]["mtu_bytes"]
    token_ready = {0: 0}
    group_times: dict[int, dict[str, int]] = {}
    flow_times: dict[int, dict[str, int]] = {}
    epoch_times: dict[tuple[int, int], dict[str, int | None]] = {}
    epoch_close: dict[tuple[int, int], int] = {}
    port_available: dict[tuple[int, int], int] = defaultdict(int)
    epoch_by_group = {
        group_id: (program["plane_id"], epoch["program_epoch_id"])
        for program in plan["plane_programs"]
        for epoch in program["epochs"]
        for group_id in epoch["flow_group_ids"]
    }
    for program in plan["plane_programs"]:
        if program["epochs"]:
            epoch_times[(program["plane_id"], 0)] = {
                "path_start": 0,
                "path_ready": 0,
                "reconfiguration_start": None,
                "reconfiguration_end": None,
            }

    unscheduled = set(range(len(plan["flow_groups"])))
    while unscheduled:
        progress = False
        for group in plan["flow_groups"]:
            group_id = group["flow_group_id"]
            if group_id in group_times:
                token_ready[group["completion_token_id"]] = group_times[group_id][
                    "complete"
                ]
        for step in plan["steps"]:
            if all(group_id in group_times for group_id in step["flow_group_ids"]):
                token_ready[step["completion_token_id"]] = max(
                    group_times[group_id]["complete"]
                    for group_id in step["flow_group_ids"]
                )

        for program in plan["plane_programs"]:
            plane_id = program["plane_id"]
            for epoch in program["epochs"]:
                epoch_id = epoch["program_epoch_id"]
                key = (plane_id, epoch_id)
                if key in epoch_times or epoch_id == 0:
                    continue
                previous_key = (plane_id, epoch_id - 1)
                gates = epoch["path_prep_not_before_token_ids"]
                if previous_key not in epoch_close or any(
                    token_id not in token_ready for token_id in gates
                ):
                    continue
                path_start = max(
                    [epoch_close[previous_key]]
                    + [token_ready[token_id] for token_id in gates]
                )
                if epoch["transition"] == "reconfigure":
                    path_ready = (
                        path_start
                        + plan["topology"]["reconfiguration_delay_ps"]
                    )
                    reconfiguration_start = path_start
                    reconfiguration_end = path_ready
                else:
                    path_ready = path_start
                    reconfiguration_start = reconfiguration_end = None
                epoch_times[key] = {
                    "path_start": path_start,
                    "path_ready": path_ready,
                    "reconfiguration_start": reconfiguration_start,
                    "reconfiguration_end": reconfiguration_end,
                }
                progress = True

        for group_id in sorted(unscheduled):
            group = plan["flow_groups"][group_id]
            epoch_key = epoch_by_group[group_id]
            dependencies = group["depends_on_token_ids"]
            if epoch_key not in epoch_times or any(
                token_id not in token_ready for token_id in dependencies
            ):
                continue
            dependency_ready = max(token_ready[token_id] for token_id in dependencies)
            release = max(
                dependency_ready, int(epoch_times[epoch_key]["path_ready"])
            )
            if plan["path_preparation_policy"] == "step_lockstep":
                step_id = group["step_id"]
                step_path_times = [
                    epoch_times[epoch_by_group[member_id]]["path_ready"]
                    for member_id in plan["steps"][step_id]["flow_group_ids"]
                    if epoch_by_group[member_id] in epoch_times
                ]
                if len(step_path_times) != len(
                    plan["steps"][step_id]["flow_group_ids"]
                ):
                    continue
                release = max(release, *(int(value) for value in step_path_times))

            received_times = []
            for flow_id in group["flow_ids"]:
                flow = plan["flows"][flow_id]
                port_key = (group["plane_id"], flow["src_rank"])
                serializer_start = max(release, port_available[port_key])
                numerator = flow["payload_bytes"] * 8 * 10**12
                denominator = plan["topology"]["per_plane_bps"]
                serialization_ps = (numerator + denominator - 1) // denominator
                last_sent = serializer_start + serialization_ps
                last_received = last_sent + plan["topology"]["data_latency_ps"]
                port_available[port_key] = last_sent
                flow_times[flow_id] = {
                    "last_received": last_received,
                    "last_sent": last_sent,
                    "serializer_start": serializer_start,
                }
                received_times.append(last_received)
            group_times[group_id] = {
                "complete": max(received_times),
                "dependency_ready": dependency_ready,
                "release": release,
            }
            unscheduled.remove(group_id)
            progress = True

        for program in plan["plane_programs"]:
            plane_id = program["plane_id"]
            for epoch in program["epochs"]:
                key = (plane_id, epoch["program_epoch_id"])
                if key in epoch_close and epoch["flow_group_ids"]:
                    continue
                if all(
                    group_id in group_times for group_id in epoch["flow_group_ids"]
                ):
                    epoch_close[key] = max(
                        group_times[group_id]["complete"]
                        for group_id in epoch["flow_group_ids"]
                    )
                    progress = True
        if not progress and unscheduled:
            raise ValueError(f"fixture schedule is blocked: {sorted(unscheduled)}")

    # Publish any completion tokens produced in the last loop iteration.
    for group in plan["flow_groups"]:
        token_ready[group["completion_token_id"]] = group_times[
            group["flow_group_id"]
        ]["complete"]
    for step in plan["steps"]:
        token_ready[step["completion_token_id"]] = max(
            group_times[group_id]["complete"]
            for group_id in step["flow_group_ids"]
        )

    flow_results = []
    for flow in plan["flows"]:
        flow_id = flow["flow_id"]
        group_id = flow["flow_group_id"]
        release = group_times[group_id]["release"]
        packets = math.ceil(flow["payload_bytes"] / mtu)
        flow_results.append(
            {
                "dst_rank": flow["dst_rank"],
                "flow_group_id": group_id,
                "flow_id": flow_id,
                "last_payload_received_ps": flow_times[flow_id]["last_received"],
                "last_payload_sent_ps": flow_times[flow_id]["last_sent"],
                "logical_packet_count": packets,
                "payload_bytes": flow["payload_bytes"],
                "received_payload_bytes": flow["payload_bytes"],
                "release_ps": release,
                "sent_payload_bytes": flow["payload_bytes"],
                "src_rank": flow["src_rank"],
                "status": "complete",
            }
        )

    group_results = []
    for group in plan["flow_groups"]:
        group_id = group["flow_group_id"]
        member_flows = [flow_results[flow_id] for flow_id in group["flow_ids"]]
        payload = sum(flow["payload_bytes"] for flow in member_flows)
        dependency_ready = max(token_ready[token] for token in group["depends_on_token_ids"])
        group_results.append(
            {
                "completed_flow_count": len(member_flows),
                "completion_ps": group_times[group_id]["complete"],
                "configuration_id": group["configuration_id"],
                "dependency_ready_ps": dependency_ready,
                "depends_on_token_ids": group["depends_on_token_ids"],
                "expected_flow_count": len(member_flows),
                "expected_payload_bytes": payload,
                "flow_group_id": group_id,
                "flow_ids": group["flow_ids"],
                "physical_config_generation": None,
                "plane_id": group["plane_id"],
                "program_epoch_id": group["program_epoch_id"],
                "received_payload_bytes": payload,
                "release_ps": group_times[group_id]["release"],
                "sent_payload_bytes": payload,
                "status": "complete",
                "step_id": group["step_id"],
            }
        )

    step_results = []
    for step in plan["steps"]:
        releases = [group_times[group_id]["release"] for group_id in step["flow_group_ids"]]
        completes = [group_times[group_id]["complete"] for group_id in step["flow_group_ids"]]
        step_results.append(
            {
                "completed_flow_group_count": len(step["flow_group_ids"]),
                "completion_ps": max(completes),
                "completion_token_id": step["completion_token_id"],
                "expected_flow_group_count": len(step["flow_group_ids"]),
                "first_flow_group_release_ps": min(releases),
                "flow_group_ids": step["flow_group_ids"],
                "status": "complete",
                "step_id": step["step_id"],
            }
        )

    plane_results = []
    plane_expected = []
    rank_count = plan["topology"]["node_count"]
    for program in plan["plane_programs"]:
        plane_id = program["plane_id"]
        plane_groups = [group for group in group_results if group["plane_id"] == plane_id]
        source_ports = []
        plane_intervals: list[tuple[int, int]] = []
        for rank in range(rank_count):
            rank_flows = [
                flow
                for flow in flow_results
                if flow["src_rank"] == rank
                and group_results[flow["flow_group_id"]]["plane_id"] == plane_id
            ]
            intervals = [
                {
                    "end_ps": flow["last_payload_sent_ps"],
                    "start_ps": flow_times[flow["flow_id"]]["serializer_start"],
                    "truncated_at_stop": False,
                }
                for flow in rank_flows
            ]
            plane_intervals.extend(
                (interval["start_ps"], interval["end_ps"]) for interval in intervals
            )
            source_ports.append(
                {
                    "busy_intervals": intervals,
                    "busy_time_ps": sum(
                        interval["end_ps"] - interval["start_ps"]
                        for interval in intervals
                    ),
                    "logical_packet_count": sum(
                        flow["logical_packet_count"] for flow in rank_flows
                    ),
                    "max_backlog_bytes": max(
                        (flow["payload_bytes"] for flow in rank_flows), default=0
                    ),
                    "sent_payload_bytes": sum(
                        flow["sent_payload_bytes"] for flow in rank_flows
                    ),
                    "src_rank": rank,
                }
            )

        epochs = []
        previous_close = 0
        generation = 0
        for epoch in program["epochs"]:
            epoch_id = epoch["program_epoch_id"]
            epoch_groups = [group_results[group_id] for group_id in epoch["flow_group_ids"]]
            epoch_flows = [
                flow_results[flow_id]
                for group in epoch_groups
                for flow_id in group["flow_ids"]
            ]
            schedule = epoch_times[(plane_id, epoch_id)]
            path_start = schedule["path_start"]
            path_ready = schedule["path_ready"]
            reconfiguration_start = schedule["reconfiguration_start"]
            reconfiguration_end = schedule["reconfiguration_end"]
            if epoch["transition"] == "reconfigure":
                generation += 1
            transfer_start = min(
                flow_times[flow["flow_id"]]["serializer_start"]
                for flow in epoch_flows
            )
            transfer_complete = max(group["completion_ps"] for group in epoch_groups)
            close = transfer_complete
            epoch_intervals = sorted(
                (
                    flow_times[flow["flow_id"]]["serializer_start"],
                    flow["last_payload_sent_ps"],
                )
                for flow in epoch_flows
            )
            epochs.append(
                {
                    "busy_time_ps": interval_union_length(epoch_intervals),
                    "close_ps": close,
                    "configuration_id": epoch["configuration_id"],
                    "drain_complete_ps": transfer_complete,
                    "flow_group_ids": epoch["flow_group_ids"],
                    "max_serializer_backlog_bytes": max(
                        flow["payload_bytes"] for flow in epoch_flows
                    ),
                    "path_prep_not_before_token_ids": epoch[
                        "path_prep_not_before_token_ids"
                    ],
                    "path_prep_start_ps": path_start,
                    "path_ready_ps": path_ready,
                    "physical_config_generation": generation,
                    "program_epoch_id": epoch_id,
                    "reconfiguration_end_ps": reconfiguration_end,
                    "reconfiguration_start_ps": reconfiguration_start,
                    "status": "closed",
                    "transfer_complete_ps": transfer_complete,
                    "transfer_start_ps": transfer_start,
                    "transition": epoch["transition"],
                }
            )
            previous_close = close
        expected = sum(group["expected_payload_bytes"] for group in plane_groups)
        plane_expected.append(expected)
        plane_results.append(
            {
                "busy_time_ps": interval_union_length(sorted(plane_intervals)),
                "epochs": epochs,
                "expected_payload_bytes": expected,
                "max_serializer_backlog_bytes": max(
                    (port["max_backlog_bytes"] for port in source_ports), default=0
                ),
                "plane_id": plane_id,
                "received_payload_bytes": expected,
                "sent_payload_bytes": expected,
                "source_ports": source_ports,
            }
        )
        for group in plane_groups:
            group["physical_config_generation"] = epochs[
                group["program_epoch_id"]
            ]["physical_config_generation"]

    expected_payload = sum(flow["payload_bytes"] for flow in flow_results)
    logical_packets = sum(flow["logical_packet_count"] for flow in flow_results)
    per_rank_sent = [0] * rank_count
    per_rank_received = [0] * rank_count
    for flow in flow_results:
        per_rank_sent[flow["src_rank"]] += flow["sent_payload_bytes"]
        per_rank_received[flow["dst_rank"]] += flow["received_payload_bytes"]
    cct = max(group["completion_ps"] for group in group_results)
    result = {
        "blocked_state": None,
        "case_id": plan["case_id"],
        "completion_semantics": plan["transport"]["completion_semantics"],
        "dependency_mode": plan["dependency_mode"],
        "error": None,
        "execution_mode": plan["execution_mode"],
        "flow_groups": group_results,
        "flows": flow_results,
        "invariants": {name: True for name in MANDATORY_INVARIANTS},
        "path_preparation_policy": plan["path_preparation_policy"],
        "plan_file_sha256": plan_sha,
        "planes": plane_results,
        "provenance": result_provenance(),
        "run_limits": plan["run_limits"],
        "schema_version": "swot-simulation-result/v2",
        "status": "success",
        "steps": step_results,
        "stop_reason": "collective_complete",
        "strategy": plan["strategy"],
        "timing": {
            "collective_complete_ps": cct,
            "collective_start_ps": 0,
            "simulated_cct_ps": cct,
            "simulation_stop_ps": cct,
        },
        "tokens": [
            {
                "producer_id": token["producer_id"],
                "ready_ps": token_ready[token["token_id"]],
                "status": "ready",
                "token_id": token["token_id"],
                "token_type": token["token_type"],
            }
            for token in plan["readiness_tokens"]
        ],
        "traffic": {
            "completed_flow_count": len(flow_results),
            "completed_flow_group_count": len(group_results),
            "drops": 0,
            "duplicate_payload_bytes": 0,
            "expected_flow_count": len(flow_results),
            "expected_flow_group_count": len(group_results),
            "expected_payload_bytes": expected_payload,
            "in_flight_transit_unit_count": 0,
            "logical_packet_count": logical_packets,
            "missing_payload_bytes": 0,
            "per_plane_payload_bytes": plane_expected,
            "per_rank_received_payload_bytes": per_rank_received,
            "per_rank_sent_payload_bytes": per_rank_sent,
            "processed_event_count": logical_packets * 2 + len(plan["readiness_tokens"]),
            "received_payload_bytes": expected_payload,
            "sent_payload_bytes": expected_payload,
            "simulated_transit_unit_count": logical_packets,
            "unfinished_flow_ids": [],
            "wire_bytes": expected_payload,
        },
        "transport_mode": plan["transport"]["mode"],
        "units": plan["units"],
        "wire_model": plan["transport"]["wire_model"],
    }
    validate_simulation_result(result)
    return result


def make_preexecution_failure(
    stop_reason: str, error_code: str, *, plan: dict[str, Any] | None = None, plan_sha: str | None = None
) -> dict[str, Any]:
    trusted = plan is not None
    return {
        "blocked_state": None,
        "case_id": plan["case_id"] if trusted else None,
        "completion_semantics": plan["transport"]["completion_semantics"] if trusted else None,
        "dependency_mode": plan["dependency_mode"] if trusted else None,
        "error": {
            "entity_id": None,
            "entity_type": "none",
            "error_code": error_code,
            "json_pointer": None,
            "message": error_code.replace("_", " "),
            "plane_id": None,
        },
        "execution_mode": plan["execution_mode"] if trusted else None,
        "flow_groups": [],
        "flows": [],
        "invariants": {name: None for name in MANDATORY_INVARIANTS},
        "path_preparation_policy": plan["path_preparation_policy"] if trusted else None,
        "plan_file_sha256": plan_sha,
        "planes": [],
        "provenance": result_provenance() if trusted else None,
        "run_limits": plan["run_limits"] if trusted else None,
        "schema_version": "swot-simulation-result/v2",
        "status": "failure",
        "steps": [],
        "stop_reason": stop_reason,
        "strategy": plan["strategy"] if trusted else None,
        "timing": {
            "collective_complete_ps": None,
            "collective_start_ps": None,
            "simulated_cct_ps": None,
            "simulation_stop_ps": None,
        },
        "tokens": [],
        "traffic": None,
        "transport_mode": plan["transport"]["mode"] if trusted else None,
        "units": plan["units"] if trusted else None,
        "wire_model": plan["transport"]["wire_model"] if trusted else None,
    }


def make_runtime_failure(
    plan: dict[str, Any], plan_sha: str, stop_reason: str, error_code: str
) -> dict[str, Any]:
    success = make_success_result(plan, plan_sha)
    for token in success["tokens"]:
        if token["token_id"] != 0:
            token["status"] = "pending"
            token["ready_ps"] = None
    for step in success["steps"]:
        step.update(
            completed_flow_group_count=0,
            first_flow_group_release_ps=None,
            completion_ps=None,
            status="pending",
        )
    for group in success["flow_groups"]:
        group.update(
            physical_config_generation=None,
            dependency_ready_ps=(
                0 if group["depends_on_token_ids"] == [0] else None
            ),
            release_ps=None,
            completion_ps=None,
            completed_flow_count=0,
            sent_payload_bytes=0,
            received_payload_bytes=0,
            status="pending",
        )
    for flow in success["flows"]:
        flow.update(
            release_ps=None,
            last_payload_sent_ps=None,
            last_payload_received_ps=None,
            sent_payload_bytes=0,
            received_payload_bytes=0,
            logical_packet_count=0,
            status="pending",
        )
    for plane in success["planes"]:
        plane.update(sent_payload_bytes=0, received_payload_bytes=0, busy_time_ps=0, max_serializer_backlog_bytes=0)
        for port in plane["source_ports"]:
            port.update(sent_payload_bytes=0, logical_packet_count=0, max_backlog_bytes=0, busy_time_ps=0, busy_intervals=[])
        for epoch in plane["epochs"]:
            epoch.update(
                physical_config_generation=0 if epoch["program_epoch_id"] == 0 else None,
                path_prep_start_ps=0 if epoch["program_epoch_id"] == 0 else None,
                path_ready_ps=0 if epoch["program_epoch_id"] == 0 else None,
                reconfiguration_start_ps=None,
                reconfiguration_end_ps=None,
                transfer_start_ps=None,
                transfer_complete_ps=None,
                drain_complete_ps=None,
                close_ps=None,
                busy_time_ps=0,
                max_serializer_backlog_bytes=0,
                status="path_ready" if epoch["program_epoch_id"] == 0 else "pending",
            )
    expected_payload = sum(flow["payload_bytes"] for flow in success["flows"])
    success.update(
        status="failure",
        stop_reason=stop_reason,
        error={
            "entity_id": None,
            "entity_type": "none",
            "error_code": error_code,
            "json_pointer": None,
            "message": error_code.replace("_", " "),
            "plane_id": None,
        },
        blocked_state={
            "next_event_time_ps": None,
            "planes": [
                {
                    "active_configuration_id": program["epochs"][0]["configuration_id"] if program["epochs"] else None,
                    "active_program_epoch_id": 0 if program["epochs"] else None,
                    "in_flight_transit_unit_count": 0,
                    "pending_path_prep_token_ids": [],
                    "physical_config_generation": 0 if program["epochs"] else None,
                    "plane_id": program["plane_id"],
                    "program_cursor": 0,
                    "serializer_backlog_flow_count": 0,
                    "state": "ready_reserved" if program["epochs"] else "program_complete",
                }
                for program in plan["plane_programs"]
            ],
            "unfinished_flow_group_ids": list(range(len(plan["flow_groups"]))),
            "unfinished_flow_ids": list(range(len(plan["flows"]))),
            "unfinished_step_ids": list(range(len(plan["steps"]))),
            "unfinished_token_ids": list(range(1, len(plan["readiness_tokens"]))),
        },
        timing={
            "collective_complete_ps": None,
            "collective_start_ps": 0,
            "simulated_cct_ps": None,
            "simulation_stop_ps": 0,
        },
        invariants={name: None for name in MANDATORY_INVARIANTS},
    )
    success["traffic"].update(
        completed_flow_count=0,
        completed_flow_group_count=0,
        sent_payload_bytes=0,
        received_payload_bytes=0,
        wire_bytes=0,
        logical_packet_count=0,
        simulated_transit_unit_count=0,
        processed_event_count=0,
        missing_payload_bytes=expected_payload,
        unfinished_flow_ids=list(range(len(plan["flows"]))),
        per_rank_sent_payload_bytes=[0] * plan["topology"]["node_count"],
        per_rank_received_payload_bytes=[0] * plan["topology"]["node_count"],
    )
    validate_simulation_result(success)
    return success


def fixture_specs() -> dict[str, dict[str, Any]]:
    swap2 = [[1, 0]]
    configs4 = [[1, 0, 3, 2], [2, 3, 0, 1]]
    single_group = [
        {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 0, "dst_rank": 1, "payload_bytes": 1000}
    ]
    single_program = [[{"transition": "initial", "configuration_id": 0, "flow_group_ids": [0]}]]
    specs: dict[str, dict[str, Any]] = {
        "one_plane_one_group": dict(strategy="swot", dependency_mode="global_step_barrier", node_count=2, plane_count=1, configurations=swap2, group_specs=single_group, program_specs=single_program),
        "sparse_permutation_without_fake_flow": dict(strategy="swot", dependency_mode="global_step_barrier", node_count=4, plane_count=1, configurations=[configs4[0]], group_specs=single_group, program_specs=single_program),
        "two_groups_same_epoch_independent_release": dict(
            strategy="swot", dependency_mode="explicit_group_dag", node_count=4, plane_count=1, configurations=[configs4[0]],
            group_specs=[
                {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 0, "dst_rank": 1, "payload_bytes": 1000},
                {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 2, "dst_rank": 3, "payload_bytes": 2000},
                {"step_id": 1, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 1, "dst_rank": 0, "payload_bytes": 1000, "parent_groups": [0]},
            ],
            program_specs=[[{"transition": "initial", "configuration_id": 0, "flow_group_ids": [0, 1, 2]}]],
        ),
        "global_step_barrier": dict(
            strategy="swot", dependency_mode="global_step_barrier", node_count=2, plane_count=1, configurations=swap2,
            group_specs=[
                {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 0, "dst_rank": 1, "payload_bytes": 1000},
                {"step_id": 1, "plane_id": 0, "program_epoch_id": 1, "configuration_id": 0, "src_rank": 1, "dst_rank": 0, "payload_bytes": 1000},
            ],
            program_specs=[[{"transition": "initial", "configuration_id": 0, "flow_group_ids": [0]}, {"transition": "retain", "configuration_id": 0, "flow_group_ids": [1]}]],
        ),
        "explicit_group_dag": dict(
            strategy="swot", dependency_mode="explicit_group_dag", node_count=2, plane_count=1, configurations=swap2,
            group_specs=[
                {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 0, "dst_rank": 1, "payload_bytes": 1000},
                {"step_id": 1, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 1, "dst_rank": 0, "payload_bytes": 1000, "parent_groups": [0]},
            ],
            program_specs=[[{"transition": "initial", "configuration_id": 0, "flow_group_ids": [0, 1]}]],
        ),
        "baseline_step_lockstep": dict(
            strategy="baseline", dependency_mode="global_step_barrier", node_count=4, plane_count=2, configurations=configs4,
            group_specs=[
                {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 0, "dst_rank": 1, "payload_bytes": 1000},
                {"step_id": 0, "plane_id": 1, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 2, "dst_rank": 3, "payload_bytes": 1000},
                {"step_id": 1, "plane_id": 0, "program_epoch_id": 1, "configuration_id": 1, "src_rank": 0, "dst_rank": 2, "payload_bytes": 1000},
                {"step_id": 1, "plane_id": 1, "program_epoch_id": 1, "configuration_id": 1, "src_rank": 1, "dst_rank": 3, "payload_bytes": 1000},
            ],
            program_specs=[
                [{"transition": "initial", "configuration_id": 0, "flow_group_ids": [0], "path_gate": "previous_step"}, {"transition": "reconfigure", "configuration_id": 1, "flow_group_ids": [2], "path_gate": "previous_step"}],
                [{"transition": "initial", "configuration_id": 0, "flow_group_ids": [1], "path_gate": "previous_step"}, {"transition": "reconfigure", "configuration_id": 1, "flow_group_ids": [3], "path_gate": "previous_step"}],
            ],
        ),
        "swot_overlap_earliest": dict(
            strategy="swot", dependency_mode="global_step_barrier", node_count=4, plane_count=1, configurations=configs4,
            group_specs=[
                {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 0, "dst_rank": 1, "payload_bytes": 1000},
                {"step_id": 1, "plane_id": 0, "program_epoch_id": 1, "configuration_id": 1, "src_rank": 0, "dst_rank": 2, "payload_bytes": 1000},
            ],
            program_specs=[[{"transition": "initial", "configuration_id": 0, "flow_group_ids": [0]}, {"transition": "reconfigure", "configuration_id": 1, "flow_group_ids": [1]}]],
        ),
        "one_shot_static_preinstalled": dict(
            strategy="one_shot", dependency_mode="global_step_barrier", node_count=4, plane_count=1, configurations=[configs4[0]],
            group_specs=[
                {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 0, "dst_rank": 1, "payload_bytes": 1000},
                {"step_id": 1, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 2, "dst_rank": 3, "payload_bytes": 1000},
            ],
            program_specs=[[{"transition": "initial", "configuration_id": 0, "flow_group_ids": [0, 1]}]],
        ),
        "retain_new_epoch_same_generation": dict(
            strategy="swot", dependency_mode="global_step_barrier", node_count=2, plane_count=1, configurations=swap2,
            group_specs=[
                {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 0, "dst_rank": 1, "payload_bytes": 1000},
                {"step_id": 1, "plane_id": 0, "program_epoch_id": 1, "configuration_id": 0, "src_rank": 1, "dst_rank": 0, "payload_bytes": 1000},
            ],
            program_specs=[[{"transition": "initial", "configuration_id": 0, "flow_group_ids": [0]}, {"transition": "retain", "configuration_id": 0, "flow_group_ids": [1]}]],
        ),
        "reconfigure_new_generation": dict(
            strategy="swot", dependency_mode="global_step_barrier", node_count=4, plane_count=1, configurations=configs4,
            group_specs=[
                {"step_id": 0, "plane_id": 0, "program_epoch_id": 0, "configuration_id": 0, "src_rank": 0, "dst_rank": 1, "payload_bytes": 1000},
                {"step_id": 1, "plane_id": 0, "program_epoch_id": 1, "configuration_id": 1, "src_rank": 0, "dst_rank": 2, "payload_bytes": 1000},
            ],
            program_specs=[[{"transition": "initial", "configuration_id": 0, "flow_group_ids": [0]}, {"transition": "reconfigure", "configuration_id": 1, "flow_group_ids": [1]}]],
        ),
        "exact_tail": dict(strategy="swot", dependency_mode="global_step_barrier", node_count=2, plane_count=1, configurations=swap2, group_specs=[{**single_group[0], "payload_bytes": 1501}], program_specs=single_program, mtu_bytes=1500),
        "payload_5gib": dict(strategy="swot", dependency_mode="global_step_barrier", node_count=2, plane_count=1, configurations=swap2, group_specs=[{**single_group[0], "payload_bytes": 5 * 1024**3}], program_specs=single_program),
    }
    return specs


def build() -> None:
    plans: dict[str, dict[str, Any]] = {}
    results: dict[str, dict[str, Any]] = {}
    for name, spec in fixture_specs().items():
        plan = make_plan(name, **spec)
        plan_path = PLAN_DIR / f"{name}.json"
        write(plan_path, plan)
        plan_sha = file_sha256(plan_path)
        result = make_success_result(plan, plan_sha)
        result_path = SUCCESS_DIR / f"{name}.json"
        write(result_path, result)
        plans[name] = plan
        results[name] = result
        checks = [
            {"plan_json_pointer": "/case_id", "result_json_pointer": "/case_id"},
            {"plan_json_pointer": "/dependency_mode", "result_json_pointer": "/dependency_mode"},
            {"plan_json_pointer": "/execution_mode", "result_json_pointer": "/execution_mode"},
            {"plan_json_pointer": "/path_preparation_policy", "result_json_pointer": "/path_preparation_policy"},
            {"plan_json_pointer": "/run_limits", "result_json_pointer": "/run_limits"},
            {"plan_json_pointer": "/strategy", "result_json_pointer": "/strategy"},
            {"plan_json_pointer": "/transport/completion_semantics", "result_json_pointer": "/completion_semantics"},
            {"plan_json_pointer": "/transport/mode", "result_json_pointer": "/transport_mode"},
            {"plan_json_pointer": "/transport/wire_model", "result_json_pointer": "/wire_model"},
            {"plan_json_pointer": "/units", "result_json_pointer": "/units"},
        ]
        pair = {
            "equality_checks": checks,
            "expected_status": "success",
            "fixture_id": name,
            "plan_file_sha256": plan_sha,
            "plan_path": f"plans/valid/{name}.json",
            "result_file_sha256": file_sha256(result_path),
            "result_path": f"results/success/{name}.json",
            "schema_version": "overlap4ocs-contract-pair/v1",
        }
        write(PAIR_DIR / f"{name}.json", pair)
        validate_contract_pair(pair, ROOT)

    base = plans["one_plane_one_group"]
    base_path = PLAN_DIR / "one_plane_one_group.json"
    base_sha = file_sha256(base_path)
    failures = {
        "invalid_input": make_preexecution_failure("invalid_input", "schema_validation_error", plan_sha=base_sha),
        "unsupported_semantics": make_preexecution_failure("unsupported_semantics", "unsupported_execution_mode", plan=base, plan_sha=base_sha),
        "deadlock": make_runtime_failure(base, base_sha, "deadlock", "no_progress"),
        "invariant_violation": make_runtime_failure(base, base_sha, "invariant_violation", "byte_mismatch"),
        "stale_physical_generation": make_runtime_failure(base, base_sha, "invariant_violation", "stale_physical_generation"),
        "max_simulation_time": make_runtime_failure(base, base_sha, "max_simulation_time", "max_simulation_time_reached"),
        "max_event_count": make_runtime_failure(base, base_sha, "max_event_count", "max_event_count_reached"),
    }
    for name, result in failures.items():
        validate_simulation_result(result)
        write(FAILURE_DIR / f"{name}.json", result)

    invalid_plans: dict[str, dict[str, Any]] = {}
    candidate = copy.deepcopy(base)
    candidate["flows"][0]["flow_id"] = 1
    invalid_plans["non_contiguous_id"] = candidate
    candidate = copy.deepcopy(plans["two_groups_same_epoch_independent_release"])
    candidate["flows"][1]["flow_id"] = 0
    invalid_plans["duplicate_id"] = candidate
    candidate = copy.deepcopy(base)
    candidate["flows"][0]["dst_rank"] = 0
    candidate["configurations"][0]["permutation"] = [0, 1]
    invalid_plans["self_flow"] = candidate
    candidate = copy.deepcopy(plans["sparse_permutation_without_fake_flow"])
    candidate["flows"][0]["dst_rank"] = 2
    invalid_plans["flow_route_mismatch"] = candidate
    candidate = copy.deepcopy(base)
    candidate["flow_groups"][0]["program_epoch_id"] = 1
    invalid_plans["group_epoch_mismatch"] = candidate
    candidate = copy.deepcopy(base)
    candidate["plane_programs"][0]["epochs"][0]["transition"] = "retain"
    invalid_plans["invalid_initial_transition"] = candidate
    candidate = copy.deepcopy(base)
    candidate["path_preparation_policy"] = "step_lockstep"
    invalid_plans["strategy_policy_mismatch"] = candidate
    candidate = make_plan("invalid-baseline", **fixture_specs()["baseline_step_lockstep"])
    candidate["plane_programs"][0]["epochs"][1]["path_prep_not_before_token_ids"] = []
    invalid_plans["baseline_path_prep_too_early"] = candidate
    candidate = copy.deepcopy(base)
    candidate["flows"][0]["payload_bytes"] = 0
    candidate["flows"][0]["segment_slices"][0]["length_bytes"] = 0
    invalid_plans["schema_zero_payload"] = candidate
    candidate = copy.deepcopy(base)
    candidate["configurations"][0]["permutation"] = [1, 1]
    invalid_plans["invalid_permutation"] = candidate
    candidate = copy.deepcopy(base)
    candidate["flows"][0]["segment_slices"][0]["length_bytes"] = 999
    invalid_plans["flow_slice_byte_mismatch"] = candidate
    candidate = copy.deepcopy(base)
    candidate["plane_programs"][0]["epochs"][0]["path_prep_not_before_token_ids"] = [1]
    invalid_plans["event_wait_graph_cycle"] = candidate
    candidate = copy.deepcopy(base)
    candidate["flow_groups"][0]["depends_on_token_ids"] = [1]
    invalid_plans["cyclic_token"] = candidate
    candidate = copy.deepcopy(base)
    candidate["unexpected"] = True
    invalid_plans["schema_unknown_field"] = candidate
    for name, plan in invalid_plans.items():
        write(INVALID_PLAN_DIR / f"{name}.json", plan)

    invalid_results = {}
    candidate = copy.deepcopy(results["one_plane_one_group"])
    candidate["unexpected"] = True
    invalid_results["schema_unknown_field"] = candidate
    candidate = copy.deepcopy(results["one_plane_one_group"])
    candidate["timing"]["simulated_cct_ps"] = None
    invalid_results["success_missing_timing"] = candidate
    candidate = copy.deepcopy(failures["deadlock"])
    candidate["timing"]["simulated_cct_ps"] = 1
    invalid_results["failure_has_cct"] = candidate
    candidate = copy.deepcopy(results["one_plane_one_group"])
    candidate["traffic"]["received_payload_bytes"] -= 1
    invalid_results["result_aggregate_mismatch"] = candidate
    candidate = copy.deepcopy(failures["invalid_input"])
    candidate["plan_file_sha256"] = "bad"
    invalid_results["schema_bad_hash"] = candidate
    for name, result in invalid_results.items():
        write(INVALID_RESULT_DIR / f"{name}.json", result)

    abi_limits_path = REPO_ROOT / "simulator" / "contracts" / "abi-limits-v2.json"
    limits = read_json_file(abi_limits_path)
    projection = {
        "abi_limits": {
            "abi_limits_raw_sha256": file_sha256(abi_limits_path),
            **limits,
        },
        "capability_schema_id": "htsim-ocs-capabilities/v1",
        "dependency_modes": ["explicit_group_dag", "global_step_barrier"],
        "execution_modes": ["exact_coalesced", "full_packet"],
        "operation_event_schema": {
            "raw_sha256": file_sha256(SCHEMA_FILES["htsim-ocs-operation-event/v1"]),
            "schema_id": "htsim-ocs-operation-event/v1",
        },
        "path_preparation_policies": ["overlap_earliest", "static_preinstalled", "step_lockstep"],
        "plan_schema": {
            "raw_sha256": file_sha256(SCHEMA_FILES["swot-execution-plan/v2"]),
            "schema_id": "swot-execution-plan/v2",
        },
        "result_schema": {
            "raw_sha256": file_sha256(SCHEMA_FILES["swot-simulation-result/v2"]),
            "schema_id": "swot-simulation-result/v2",
        },
        "schema_version": "overlap4ocs-htsim-capabilities-static-projection/v1",
        "transport_modes": ["paper_exact"],
    }
    validate_capabilities_projection(projection)
    write(CAPABILITY_DIR / "capabilities-v1-static-projection.json", projection)

    checksum_lines = []
    for path in sorted(ROOT.rglob("*.json")):
        checksum_lines.append(f"{file_sha256(path)}  {path.relative_to(ROOT).as_posix()}")
    (ROOT / "SHA256SUMS").write_text("\n".join(checksum_lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    build()
