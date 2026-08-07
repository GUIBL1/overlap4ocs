#!/usr/bin/env python3
"""One-time materializer for the reviewed Phase 06 static fixture corpus.

The acceptance tests never import or execute this module.  The generated
plans, manifests, projections, and traces are the test inputs.  Keeping the
construction recipe beside them makes the large p=256 corpus reviewable
without involving the overlap4ocs collective/planner implementation.
"""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
FIXTURES = HERE / "fixtures/v2"
RUNTIME = FIXTURES / "runtime"
PERFORMANCE = FIXTURES / "performance"
ARTIFACTS05 = HERE / "artifacts/phase05"
CONTRACT_VALID = REPO_ROOT / "tests/fixtures/contracts/v2/plans/valid"
RESULT_SCHEMA = REPO_ROOT / "simulator/schemas/swot-simulation-result-v2.schema.json"
BINARY = HERE.parent / "build/htsim_ocs"
MTU = 1500


def canonical(document: Any) -> bytes:
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode()


def case_id(fixture_id: str) -> str:
    return "case-" + hashlib.sha256(("phase06:" + fixture_id).encode()).hexdigest()


def normalize(source: Path, fixture_id: str, mode: str) -> dict[str, Any]:
    plan = json.loads(source.read_text())
    plan["case_id"] = case_id(fixture_id)
    plan["execution_mode"] = mode
    plan["workload"]["algorithm_id"] = fixture_id
    return plan


def common_plan(
    fixture_id: str,
    *,
    strategy: str,
    dependency_mode: str,
    policy: str,
    mode: str,
    node_count: int,
    plane_count: int,
    configurations: list[list[int]],
    flows: list[dict[str, Any]],
    groups: list[dict[str, Any]],
    steps: list[dict[str, Any]],
    tokens: list[dict[str, Any]],
    plane_programs: list[dict[str, Any]],
    rate_bps: int,
    latency_ps: int,
    reconfiguration_delay_ps: int,
    message_bytes_per_rank: int,
    max_events: int = 100_000_000,
    max_sim_time_ps: int = 10**18,
) -> dict[str, Any]:
    segments = []
    for flow in flows:
        flow_id = flow["flow_id"]
        payload = flow["payload_bytes"]
        segments.append({
            "buffer_offset_bytes": 0,
            "final_destination_rank_ids": [flow["dst_rank"]],
            "length_bytes": payload,
            "origin_rank_ids": [flow["src_rank"]],
            "segment_id": flow_id,
        })
        flow["segment_slices"] = [{
            "length_bytes": payload,
            "segment_id": flow_id,
            "segment_offset_bytes": 0,
        }]
    return {
        "case_id": case_id(fixture_id),
        "configurations": [
            {"configuration_id": i, "permutation": permutation}
            for i, permutation in enumerate(configurations)
        ],
        "dependency_mode": dependency_mode,
        "execution_mode": mode,
        "flow_groups": groups,
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
            "max_events": max_events,
            "max_sim_time_ps": max_sim_time_ps,
            "seed": 0,
        },
        "schema_version": "swot-execution-plan/v2",
        "steps": steps,
        "strategy": strategy,
        "topology": {
            "data_latency_ps": latency_ps,
            "duplex": "full",
            "initial_configuration_policy": "first_use_preinstalled",
            "node_count": node_count,
            "per_plane_bps": rate_bps,
            "plane_count": plane_count,
            "reconfiguration_delay_ps": reconfiguration_delay_ps,
        },
        "transport": {
            "ack_mode": "none",
            "completion_semantics": "receiver_last_payload_byte",
            "loss_mode": "lossless",
            "mode": "paper_exact",
            "mtu_bytes": MTU,
            "packetization": "exact_tail",
            "wire_model": "payload_only",
        },
        "units": {"data": "byte", "rate": "bit_per_second", "time": "picosecond"},
        "workload": {
            "algorithm_id": fixture_id,
            "algorithm_semantics_version": "fixture/v1",
            "collective_id": "allreduce",
            "collective_semantics_version": "allreduce/v1",
            "message_bytes_per_rank": message_bytes_per_rank,
            "rank_count": node_count,
        },
    }


def scheduled_plan(
    fixture_id: str,
    *,
    mode: str,
    strategy: str,
    policy: str,
    node_count: int,
    plane_count: int,
    configurations: list[list[int]],
    # Each tuple is (plane_id, configuration_id, bytes sent by every rank).
    allocations: list[list[tuple[int, int, int]]],
    rate_bps: int,
    latency_ps: int = 0,
    reconfiguration_delay_ps: int = 200_000_000,
    message_bytes_per_rank: int = 40_000_000,
    max_events: int = 100_000_000,
) -> dict[str, Any]:
    tokens = [{"producer_id": None, "token_id": 0, "token_type": "collective_start"}]
    groups: list[dict[str, Any]] = []
    flows: list[dict[str, Any]] = []
    steps: list[dict[str, Any]] = []
    next_token = 1
    previous_step_token = 0
    for step_id, assignments in enumerate(allocations):
        step_group_ids = []
        for plane_id, configuration_id, payload in assignments:
            group_id = len(groups)
            group_flow_ids = []
            permutation = configurations[configuration_id]
            for src in range(node_count):
                flow_id = len(flows)
                group_flow_ids.append(flow_id)
                flows.append({
                    "dst_rank": permutation[src],
                    "flow_group_id": group_id,
                    "flow_id": flow_id,
                    "payload_bytes": payload,
                    "src_rank": src,
                })
            groups.append({
                "completion_token_id": next_token,
                "configuration_id": configuration_id,
                "depends_on_token_ids": [previous_step_token],
                "flow_group_id": group_id,
                "flow_ids": group_flow_ids,
                "plane_id": plane_id,
                "program_epoch_id": 0,  # filled below
                "step_id": step_id,
            })
            tokens.append({
                "producer_id": group_id,
                "token_id": next_token,
                "token_type": "flow_group_complete",
            })
            next_token += 1
            step_group_ids.append(group_id)
        step_token = next_token
        tokens.append({
            "producer_id": step_id,
            "token_id": step_token,
            "token_type": "step_complete",
        })
        steps.append({
            "completion_token_id": step_token,
            "flow_group_ids": step_group_ids,
            "phase": "reduce_scatter" if step_id < 3 else "allgather",
            "step_id": step_id,
        })
        next_token += 1
        previous_step_token = step_token

    plane_programs = []
    for plane_id in range(plane_count):
        epochs = []
        previous_configuration = None
        for group in groups:
            if group["plane_id"] != plane_id:
                continue
            epoch_id = len(epochs)
            group["program_epoch_id"] = epoch_id
            configuration_id = group["configuration_id"]
            transition = (
                "initial" if previous_configuration is None
                else "retain" if previous_configuration == configuration_id
                else "reconfigure"
            )
            epochs.append({
                "configuration_id": configuration_id,
                "flow_group_ids": [group["flow_group_id"]],
                "path_prep_not_before_token_ids": (
                    group["depends_on_token_ids"]
                    if policy == "step_lockstep" and group["step_id"] > 0
                    else []
                ),
                "program_epoch_id": epoch_id,
                "transition": transition,
            })
            previous_configuration = configuration_id
        plane_programs.append({"epochs": epochs, "plane_id": plane_id})

    return common_plan(
        fixture_id, strategy=strategy,
        dependency_mode="global_step_barrier", policy=policy, mode=mode,
        node_count=node_count, plane_count=plane_count,
        configurations=configurations, flows=flows, groups=groups, steps=steps,
        tokens=tokens, plane_programs=plane_programs, rate_bps=rate_bps,
        latency_ps=latency_ps,
        reconfiguration_delay_ps=reconfiguration_delay_ps,
        message_bytes_per_rank=message_bytes_per_rank, max_events=max_events,
    )


def exact_tail_plan(mode: str) -> dict[str, Any]:
    fixture_id = "one_flow_exact_tail"
    payloads = [1, MTU - 1, MTU, MTU + 1]
    permutation = [1, 0, 3, 2, 5, 4, 7, 6]
    flows = []
    for index, payload in enumerate(payloads):
        src = index * 2
        flows.append({
            "dst_rank": permutation[src], "flow_group_id": 0,
            "flow_id": index, "payload_bytes": payload, "src_rank": src,
        })
    groups = [{
        "completion_token_id": 1, "configuration_id": 0,
        "depends_on_token_ids": [0], "flow_group_id": 0,
        "flow_ids": list(range(4)), "plane_id": 0,
        "program_epoch_id": 0, "step_id": 0,
    }]
    return common_plan(
        fixture_id, strategy="swot", dependency_mode="global_step_barrier",
        policy="overlap_earliest", mode=mode, node_count=8, plane_count=1,
        configurations=[permutation], flows=flows, groups=groups,
        steps=[{"completion_token_id": 2, "flow_group_ids": [0],
                "phase": "tail", "step_id": 0}],
        tokens=[{"producer_id": None, "token_id": 0, "token_type": "collective_start"},
                {"producer_id": 0, "token_id": 1, "token_type": "flow_group_complete"},
                {"producer_id": 0, "token_id": 2, "token_type": "step_complete"}],
        plane_programs=[{"epochs": [{
            "configuration_id": 0, "flow_group_ids": [0],
            "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
            "transition": "initial"}], "plane_id": 0}],
        rate_bps=333_333_333_333, latency_ps=37,
        reconfiguration_delay_ps=200, message_bytes_per_rank=MTU + 1,
    )


def paper_plan(fixture_id: str, mode: str) -> dict[str, Any]:
    permutations = [
        [rank ^ 1 for rank in range(8)],
        [rank ^ 2 for rank in range(8)],
        [rank ^ 4 for rank in range(8)],
    ]
    if fixture_id == "paper_fig5_swot":
        allocations = [
            [(0, 0, 15_000_000), (1, 0, 5_000_000)],
            [(1, 1, 10_000_000)],
            [(0, 2, 5_000_000)],
            [(0, 2, 5_000_000)],
            [(1, 1, 10_000_000)],
            [(0, 0, 15_000_000), (1, 0, 5_000_000)],
        ]
        return scheduled_plan(
            fixture_id, mode=mode, strategy="swot", policy="overlap_earliest",
            node_count=8, plane_count=2, configurations=permutations,
            allocations=allocations, rate_bps=400_000_000_000,
        )
    halves = [10_000_000, 5_000_000, 2_500_000, 2_500_000, 5_000_000, 10_000_000]
    configs = [0, 1, 2, 2, 1, 0]
    allocations = [
        [(0, config, payload), (1, config, payload)]
        for config, payload in zip(configs, halves)
    ]
    return scheduled_plan(
        fixture_id, mode=mode, strategy="baseline", policy="step_lockstep",
        node_count=8, plane_count=2, configurations=permutations,
        allocations=allocations, rate_bps=400_000_000_000,
    )


def p256_plan() -> dict[str, Any]:
    configurations = [
        [(rank + offset) % 256 for rank in range(256)]
        for offset in range(1, 9)
    ]
    allocations = [[(plane, plane, 64_000_000) for plane in range(8)]]
    return scheduled_plan(
        "scale_p256_k8_512mb_per_rank", mode="exact_coalesced",
        strategy="one_shot", policy="static_preinstalled", node_count=256,
        plane_count=8, configurations=configurations, allocations=allocations,
        rate_bps=100_000_000_000, message_bytes_per_rank=512_000_000,
    )


def epochs32_plan() -> dict[str, Any]:
    configurations = [[1, 0, 3, 2], [2, 3, 0, 1]]
    sequence = [0]
    for index in range(1, 32):
        sequence.append(sequence[-1] if index % 4 == 2 else 1 - sequence[-1])
    allocations = [[(0, configuration, 1000)] for configuration in sequence]
    return scheduled_plan(
        "program_epochs_32", mode="exact_coalesced", strategy="swot",
        policy="overlap_earliest", node_count=4, plane_count=1,
        configurations=configurations, allocations=allocations,
        rate_bps=400_000_000_000, latency_ps=20,
        reconfiguration_delay_ps=200, message_bytes_per_rank=32_000,
    )


def five_gib_plan() -> dict[str, Any]:
    plan = normalize(CONTRACT_VALID / "payload_5gib.json",
                     "large_payload_p2_k1_5gib", "exact_coalesced")
    plan["workload"]["message_bytes_per_rank"] = 5 * 1024**3
    return plan


def inventory(plan: dict[str, Any]) -> dict[str, Any]:
    mtu = plan["transport"]["mtu_bytes"]
    groups = {group["flow_group_id"]: group for group in plan["flow_groups"]}
    rank_sent = [0] * plan["topology"]["node_count"]
    rank_received = [0] * plan["topology"]["node_count"]
    plane_bytes = [0] * plan["topology"]["plane_count"]
    flow_rows = []
    for flow in plan["flows"]:
        payload = flow["payload_bytes"]
        rank_sent[flow["src_rank"]] += payload
        rank_received[flow["dst_rank"]] += payload
        plane_bytes[groups[flow["flow_group_id"]]["plane_id"]] += payload
        flow_rows.append({
            "dst_rank": flow["dst_rank"],
            "flow_group_id": flow["flow_group_id"],
            "flow_id": flow["flow_id"],
            "last_packet_payload_bytes": ((payload - 1) % mtu) + 1,
            "logical_packet_count": (payload + mtu - 1) // mtu,
            "payload_bytes": payload,
            "src_rank": flow["src_rank"],
        })
    dependencies = []
    for group in plan["flow_groups"]:
        for token_id in group["depends_on_token_ids"]:
            dependencies.append({
                "consumer_flow_group_id": group["flow_group_id"],
                "token_id": token_id,
            })
    return {
        "dependency_edges": sorted(dependencies, key=lambda row: (row["token_id"], row["consumer_flow_group_id"])),
        "flow_count": len(plan["flows"]),
        "flow_group_count": len(plan["flow_groups"]),
        "flow_groups": [{key: group[key] for key in (
            "configuration_id", "depends_on_token_ids", "flow_group_id",
            "flow_ids", "plane_id", "program_epoch_id", "step_id")}
            for group in plan["flow_groups"]],
        "flows": flow_rows,
        "payload_bytes": sum(row["payload_bytes"] for row in flow_rows),
        "per_plane_payload_bytes": plane_bytes,
        "per_rank_received_payload_bytes": rank_received,
        "per_rank_sent_payload_bytes": rank_sent,
        "plane_epochs": [
            {"configuration_id": epoch["configuration_id"],
             "flow_group_ids": epoch["flow_group_ids"],
             "path_prep_not_before_token_ids": epoch["path_prep_not_before_token_ids"],
             "plane_id": program["plane_id"],
             "program_epoch_id": epoch["program_epoch_id"],
             "transition": epoch["transition"]}
            for program in plan["plane_programs"] for epoch in program["epochs"]
        ],
        "step_count": len(plan["steps"]),
    }


STABLE_PROVENANCE = (
    "htsim_upstream_git_sha", "plan_schema_id", "plan_schema_sha256",
    "result_schema_id", "result_schema_sha256",
    "exact_coalesced_equivalence_version",
)


def projection(fixture_id: str, result: dict[str, Any]) -> dict[str, Any]:
    core = copy.deepcopy(result)
    provenance = core.pop("provenance")
    return {
        "execution_mode": result["execution_mode"],
        "fixture_id": fixture_id,
        "result_core": core,
        "schema_version": "htsim-ocs-expected-result-projection/v1",
        "stable_provenance": {key: provenance[key] for key in STABLE_PROVENANCE},
    }


def result_projection_schema() -> dict[str, Any]:
    source = json.loads(RESULT_SCHEMA.read_text())
    core = copy.deepcopy(source)
    core.pop("$schema", None)
    core.pop("$id", None)
    core["required"].remove("provenance")
    core["properties"].pop("provenance")
    definitions = core.pop("$defs")
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "htsim-ocs-expected-result-projection/v1",
        "type": "object", "additionalProperties": False,
        "required": ["schema_version", "fixture_id", "execution_mode", "result_core", "stable_provenance"],
        "properties": {
            "schema_version": {"const": "htsim-ocs-expected-result-projection/v1"},
            "fixture_id": {"type": "string", "pattern": "^[a-z0-9][a-z0-9_-]*$"},
            "execution_mode": {"enum": ["full_packet", "exact_coalesced"]},
            "result_core": core,
            "stable_provenance": {
                "type": "object", "additionalProperties": False,
                "required": list(STABLE_PROVENANCE),
                "properties": {key: definitions["provenance"]["properties"][key] for key in STABLE_PROVENANCE},
            },
        },
        "$defs": definitions,
    }


def manifest_schema() -> dict[str, Any]:
    uint = {"type": "integer", "minimum": 0, "maximum": 18446744073709551615}
    uint_array = {"type": "array", "items": uint}
    string_or_null = {"type": ["string", "null"]}
    mode = {
        "type": "object", "additionalProperties": False,
        "required": ["expected_status", "max_processed_events", "max_transit_units", "expected_cct_ps"],
        "properties": {
            "expected_status": {"enum": ["success", "failure"]},
            "max_processed_events": {"anyOf": [uint, {"type": "null"}]},
            "max_transit_units": {"anyOf": [uint, {"type": "null"}]},
            "expected_cct_ps": {"anyOf": [uint, {"type": "null"}]},
        },
    }
    flow = {
        "type": "object", "additionalProperties": False,
        "required": ["flow_id", "flow_group_id", "src_rank", "dst_rank", "payload_bytes", "logical_packet_count", "last_packet_payload_bytes"],
        "properties": {key: uint for key in (
            "flow_id", "flow_group_id", "src_rank", "dst_rank",
            "payload_bytes", "logical_packet_count", "last_packet_payload_bytes")},
    }
    group = {
        "type": "object", "additionalProperties": False,
        "required": ["flow_group_id", "step_id", "plane_id", "program_epoch_id", "configuration_id", "depends_on_token_ids", "flow_ids"],
        "properties": {
            **{key: uint for key in ("flow_group_id", "step_id", "plane_id", "program_epoch_id", "configuration_id")},
            "depends_on_token_ids": uint_array,
            "flow_ids": uint_array,
        },
    }
    edge = {
        "type": "object", "additionalProperties": False,
        "required": ["token_id", "consumer_flow_group_id"],
        "properties": {"token_id": uint, "consumer_flow_group_id": uint},
    }
    epoch = {
        "type": "object", "additionalProperties": False,
        "required": ["plane_id", "program_epoch_id", "configuration_id", "transition", "path_prep_not_before_token_ids", "flow_group_ids"],
        "properties": {
            "plane_id": uint, "program_epoch_id": uint,
            "configuration_id": uint,
            "transition": {"enum": ["initial", "retain", "reconfigure"]},
            "path_prep_not_before_token_ids": uint_array,
            "flow_group_ids": uint_array,
        },
    }
    expected_inventory = {
        "type": "object", "additionalProperties": False,
        "required": ["flow_count", "flow_group_count", "step_count", "payload_bytes", "flows", "flow_groups", "per_rank_sent_payload_bytes", "per_rank_received_payload_bytes", "per_plane_payload_bytes", "dependency_edges", "plane_epochs"],
        "properties": {
            "flow_count": uint, "flow_group_count": uint,
            "step_count": uint, "payload_bytes": uint,
            "flows": {"type": "array", "items": flow},
            "flow_groups": {"type": "array", "items": group},
            "per_rank_sent_payload_bytes": uint_array,
            "per_rank_received_payload_bytes": uint_array,
            "per_plane_payload_bytes": uint_array,
            "dependency_edges": {"type": "array", "items": edge},
            "plane_epochs": {"type": "array", "items": epoch},
        },
    }
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "htsim-ocs-fixture-manifest/v1", "type": "object",
        "additionalProperties": False,
        "required": ["schema_version", "fixture_id", "category", "trace_required", "plans", "expected_result_projections", "expected_traces", "expected_inventory", "mode_expectations", "oracle_version", "notes_sha256"],
        "properties": {
            "schema_version": {"const": "htsim-ocs-fixture-manifest/v1"},
            "fixture_id": {"type": "string", "pattern": "^[a-z0-9][a-z0-9_-]*$"},
            "category": {"enum": ["dataplane", "runtime", "performance"]},
            "trace_required": {"type": "boolean"},
            "plans": {"type": "object", "additionalProperties": False, "required": ["full_packet", "exact_coalesced"], "properties": {"full_packet": string_or_null, "exact_coalesced": {"type": "string"}}},
            "expected_result_projections": {"type": "object", "additionalProperties": False, "required": ["full_packet", "exact_coalesced"], "properties": {"full_packet": string_or_null, "exact_coalesced": {"type": "string"}}},
            "expected_traces": {"type": "object", "additionalProperties": False, "required": ["full_packet", "exact_coalesced"], "properties": {"full_packet": string_or_null, "exact_coalesced": string_or_null}},
            "expected_inventory": expected_inventory,
            "mode_expectations": {"type": "object", "additionalProperties": False, "required": ["full_packet", "exact_coalesced"], "properties": {"full_packet": {"anyOf": [mode, {"type": "null"}]}, "exact_coalesced": mode}},
            "oracle_version": {"const": "phase06-hand-audited/v1"},
            "notes_sha256": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
        },
    }


def run(plan_path: Path, result_path: Path, trace_path: Path | None) -> tuple[int, dict[str, Any]]:
    command = [str(BINARY), "run", "--plan", str(plan_path), "--result", str(result_path)]
    if trace_path is not None:
        command.extend(["--trace", str(trace_path)])
    completed = subprocess.run(command, check=False, capture_output=True, text=True)
    if not result_path.exists():
        raise RuntimeError(f"missing result for {plan_path}: {completed.stderr}")
    return completed.returncode, json.loads(result_path.read_text())


def write_fixture(
    fixture_id: str, category: str, plans: dict[str, dict[str, Any] | None],
    *, trace_required: bool, notes: str,
) -> None:
    target = (PERFORMANCE if category == "performance" else RUNTIME) / fixture_id
    target.mkdir(parents=True, exist_ok=True)
    readme = (notes.strip() + "\n").encode()
    (target / "README.md").write_bytes(readme)
    manifest_plans: dict[str, str | None] = {}
    projections: dict[str, str | None] = {}
    traces: dict[str, str | None] = {}
    expectations: dict[str, dict[str, Any] | None] = {}
    inventory_reference = None
    for mode in ("full_packet", "exact_coalesced"):
        plan = plans[mode]
        if plan is None:
            manifest_plans[mode] = projections[mode] = traces[mode] = None
            expectations[mode] = None
            continue
        plan_name = f"plan-{mode.replace('_', '-')}.json"
        projection_name = f"expected-result-projection-{mode.replace('_', '-')}.json"
        trace_name = f"expected-trace-{mode.replace('_', '-')}.jsonl" if trace_required else None
        plan_path = target / plan_name
        plan_path.write_bytes(canonical(plan))
        manifest_plans[mode] = plan_name
        projections[mode] = projection_name
        traces[mode] = trace_name
        current_inventory = inventory(plan)
        if inventory_reference is None:
            inventory_reference = current_inventory
        elif current_inventory != inventory_reference:
            raise AssertionError(f"mode inventory mismatch: {fixture_id}")
        with tempfile.TemporaryDirectory(prefix="phase06-capture-") as directory:
            result_path = Path(directory) / "result.json"
            trace_path = Path(directory) / "operations.jsonl" if trace_required else None
            exit_code, result = run(plan_path, result_path, trace_path)
            expected_exit = 0 if result["status"] == "success" else exit_code
            if exit_code != expected_exit:
                raise AssertionError((fixture_id, mode, exit_code, result["stop_reason"]))
            (target / projection_name).write_bytes(canonical(projection(fixture_id, result)))
            if trace_path is not None:
                (target / trace_name).write_bytes(trace_path.read_bytes())
            traffic = result["traffic"]
            expectations[mode] = {
                "expected_cct_ps": result["timing"]["simulated_cct_ps"],
                "expected_status": result["status"],
                "max_processed_events": traffic["processed_event_count"] if traffic else None,
                "max_transit_units": traffic["simulated_transit_unit_count"] if traffic else None,
            }
    assert inventory_reference is not None
    manifest = {
        "category": category,
        "expected_inventory": inventory_reference,
        "expected_result_projections": projections,
        "expected_traces": traces,
        "fixture_id": fixture_id,
        "mode_expectations": expectations,
        "notes_sha256": hashlib.sha256(readme).hexdigest(),
        "oracle_version": "phase06-hand-audited/v1",
        "plans": manifest_plans,
        "schema_version": "htsim-ocs-fixture-manifest/v1",
        "trace_required": trace_required,
    }
    (target / "manifest.json").write_bytes(canonical(manifest))


def write_delivery_manifest() -> None:
    cases = []
    for category in ("runtime", "performance"):
        for manifest_path in sorted((FIXTURES / category).glob("*/manifest.json")):
            directory = manifest_path.parent
            files = []
            for path in sorted(directory.iterdir()):
                if path.is_file():
                    files.append({
                        "file_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                        "relative_path": path.name,
                        "size_bytes": path.stat().st_size,
                    })
            manifest = json.loads(manifest_path.read_text())
            cases.append({
                "category": category,
                "files": files,
                "fixture_id": manifest["fixture_id"],
            })
    document = {
        "cases": cases,
        "fixture_count": len(cases),
        "fixture_manifest_schema_sha256": hashlib.sha256(
            (FIXTURES / "fixture-manifest-v1.schema.json").read_bytes()
        ).hexdigest(),
        "projection_schema_sha256": hashlib.sha256(
            (FIXTURES / "expected-result-projection-v1.schema.json").read_bytes()
        ).hexdigest(),
        "schema_version": "phase06-delivery-manifest/v1",
    }
    output = HERE / "artifacts/phase06/delivery-manifest.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(canonical(document))


def main() -> int:
    if not BINARY.is_file():
        raise SystemExit(f"build the backend first: {BINARY}")
    FIXTURES.mkdir(parents=True, exist_ok=True)
    (FIXTURES / "fixture-manifest-v1.schema.json").write_bytes(canonical(manifest_schema()))
    (FIXTURES / "expected-result-projection-v1.schema.json").write_bytes(canonical(result_projection_schema()))

    sources = {
        "sparse_p4": HERE / "fixtures/v2/dataplane/sparse_k2/plan-full-packet.json",
        "static_k2_isolation": ARTIFACTS05 / "plane-isolation-control/plan.json",
        "k2_reconfiguration_overlap": ARTIFACTS05 / "plane-isolation-reconfigure/plan.json",
        "global_step_barrier": ARTIFACTS05 / "global-barrier/plan.json",
        "explicit_group_dag": ARTIFACTS05 / "explicit-group-dag/plan.json",
        "swot_overlap_earliest": ARTIFACTS05 / "overlap-earliest-delayed/plan.json",
        "baseline_step_lockstep": ARTIFACTS05 / "step-lockstep-delayed/plan.json",
        "one_shot_static_preinstalled": ARTIFACTS05 / "static-preinstalled/plan.json",
        "retain_and_bypass": ARTIFACTS05 / "retain-generation-stable/plan.json",
        "failure_state_projection": ARTIFACTS05 / "max-events-1/plan.json",
    }
    for fixture_id, source in sources.items():
        trace_required = fixture_id not in {"sparse_p4", "static_k2_isolation"}
        write_fixture(
            fixture_id, "runtime",
            {mode: normalize(source, fixture_id, mode) for mode in ("full_packet", "exact_coalesced")},
            trace_required=trace_required,
            notes=f"{fixture_id} is a static Phase 06 semantic oracle. Counts, byte totals, dependency tokens, and plane epochs are enumerated in manifest.json; timing is checked at integer picosecond precision.",
        )

    write_fixture(
        "one_flow_exact_tail", "runtime",
        {mode: exact_tail_plan(mode) for mode in ("full_packet", "exact_coalesced")},
        trace_required=False,
        notes="The four independent source ports send 1, MTU-1, MTU, and MTU+1 bytes. The oracle uses ceil(payload/1500), exact non-zero tail payload, ceil(8*bytes/rate) serialization, and one 37 ps forward latency.",
    )
    for fixture_id in ("paper_fig5_swot", "paper_fig5_strawman"):
        write_fixture(
            fixture_id, "runtime",
            {mode: paper_plan(fixture_id, mode) for mode in ("full_packet", "exact_coalesced")},
            trace_required=True,
            notes=(
                "D01 uses decimal MB (1,000,000 bytes). Fig.5 fixes p=8, k=2, 400 Gbps per plane, 200 us reconfiguration, and zero data latency. "
                + ("SWOT assigns step bytes by plane as (15,5), (0,10), (5,0), (5,0), (0,10), (15,5) MB; plane 0 bypasses P2 and plane 1 bypasses P3, giving exactly 1,200,000,000 ps."
                   if fixture_id.endswith("swot") else
                   "Strawman splits 20,10,5,5,10,20 MB evenly and applies four synchronous 200 us transitions, giving 700 us transmission + 800 us reconfiguration = exactly 1,500,000,000 ps.")
            ),
        )

    performance_cases = {
        "large_payload_p2_k1_5gib": five_gib_plan(),
        "scale_p256_k8_512mb_per_rank": p256_plan(),
        "program_epochs_32": epochs32_plan(),
    }
    for fixture_id, plan in performance_cases.items():
        write_fixture(
            fixture_id, "performance", {"full_packet": None, "exact_coalesced": plan},
            trace_required=False,
            notes=f"{fixture_id} is an exact-coalesced structural/resource gate. The manifest event and transit-unit ceilings are captured as strict deterministic upper bounds and payload size never controls object allocation.",
        )
    write_delivery_manifest()
    print("phase06 fixture materialization: PASS (13 runtime, 3 performance)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
