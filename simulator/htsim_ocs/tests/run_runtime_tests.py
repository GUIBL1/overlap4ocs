#!/usr/bin/env python3
"""Phase 05 standalone OCS runtime semantic and delivery-data tests."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[3]
CONTRACT_PLANS = REPO_ROOT / "tests/fixtures/contracts/v2/plans/valid"


def token(token_id: int, token_type: str, producer_id: int | None) -> dict[str, Any]:
    return {
        "producer_id": producer_id,
        "token_id": token_id,
        "token_type": token_type,
    }


def canonical_bytes(document: dict[str, Any]) -> bytes:
    return (
        json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")


def make_plan(
    name: str,
    *,
    strategy: str,
    dependency_mode: str,
    path_policy: str,
    execution_mode: str,
    node_count: int,
    plane_count: int,
    configurations: list[list[int]],
    tokens: list[dict[str, Any]],
    steps: list[dict[str, Any]],
    groups: list[dict[str, Any]],
    plane_epochs: list[list[dict[str, Any]]],
    rate_bps: int = 400_000_000_000,
    latency_ps: int = 20,
    reconfiguration_delay_ps: int = 200,
    mtu: int = 1500,
    max_sim_time_ps: int = 10**15,
    max_events: int = 10_000_000,
) -> dict[str, Any]:
    flows: list[dict[str, Any]] = []
    segments: list[dict[str, Any]] = []
    wire_groups: list[dict[str, Any]] = []
    for group_id, group in enumerate(groups):
        flow_id = group_id
        payload = int(group["payload"])
        segments.append(
            {
                "buffer_offset_bytes": 0,
                "final_destination_rank_ids": [int(group["dst"])],
                "length_bytes": payload,
                "origin_rank_ids": [int(group["src"])],
                "segment_id": flow_id,
            }
        )
        flows.append(
            {
                "dst_rank": int(group["dst"]),
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
                "src_rank": int(group["src"]),
            }
        )
        wire_groups.append(
            {
                "completion_token_id": int(group["completion_token_id"]),
                "configuration_id": int(group["configuration_id"]),
                "depends_on_token_ids": list(group["depends_on_token_ids"]),
                "flow_group_id": group_id,
                "flow_ids": [flow_id],
                "plane_id": int(group["plane_id"]),
                "program_epoch_id": int(group["program_epoch_id"]),
                "step_id": int(group["step_id"]),
            }
        )
    case_hash = hashlib.sha256(name.encode("utf-8")).hexdigest()
    return {
        "case_id": f"case-{case_hash}",
        "configurations": [
            {"configuration_id": index, "permutation": permutation}
            for index, permutation in enumerate(configurations)
        ],
        "dependency_mode": dependency_mode,
        "execution_mode": execution_mode,
        "flow_groups": wire_groups,
        "flows": flows,
        "logical_segments": segments,
        "path_preparation_policy": path_policy,
        "plane_programs": [
            {"epochs": epochs, "plane_id": plane_id}
            for plane_id, epochs in enumerate(plane_epochs)
        ],
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
            "mtu_bytes": mtu,
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
            "message_bytes_per_rank": max(group["payload"] for group in groups),
            "rank_count": node_count,
        },
    }


def three_epoch_plan(mode: str = "full_packet") -> dict[str, Any]:
    groups = [
        {"step_id": 0, "plane_id": 0, "program_epoch_id": 0,
         "configuration_id": 0, "depends_on_token_ids": [0],
         "completion_token_id": 1, "src": 0, "dst": 1, "payload": 1000},
        {"step_id": 1, "plane_id": 0, "program_epoch_id": 1,
         "configuration_id": 1, "depends_on_token_ids": [2],
         "completion_token_id": 3, "src": 0, "dst": 2, "payload": 1000},
        {"step_id": 2, "plane_id": 0, "program_epoch_id": 2,
         "configuration_id": 0, "depends_on_token_ids": [4],
         "completion_token_id": 5, "src": 0, "dst": 1, "payload": 1000},
    ]
    return make_plan(
        f"three-reconfigure-{mode}", strategy="swot",
        dependency_mode="global_step_barrier", path_policy="overlap_earliest",
        execution_mode=mode, node_count=4, plane_count=1,
        configurations=[[1, 0, 3, 2], [2, 3, 0, 1]],
        tokens=[token(0, "collective_start", None),
                token(1, "flow_group_complete", 0), token(2, "step_complete", 0),
                token(3, "flow_group_complete", 1), token(4, "step_complete", 1),
                token(5, "flow_group_complete", 2), token(6, "step_complete", 2)],
        steps=[
            {"completion_token_id": 2, "flow_group_ids": [0],
             "phase": "phase0", "step_id": 0},
            {"completion_token_id": 4, "flow_group_ids": [1],
             "phase": "phase1", "step_id": 1},
            {"completion_token_id": 6, "flow_group_ids": [2],
             "phase": "phase2", "step_id": 2},
        ], groups=groups,
        plane_epochs=[[
            {"configuration_id": 0, "flow_group_ids": [0],
             "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
             "transition": "initial"},
            {"configuration_id": 1, "flow_group_ids": [1],
             "path_prep_not_before_token_ids": [], "program_epoch_id": 1,
             "transition": "reconfigure"},
            {"configuration_id": 0, "flow_group_ids": [2],
             "path_prep_not_before_token_ids": [], "program_epoch_id": 2,
             "transition": "reconfigure"},
        ]],
    )


def global_k2_plan(policy: str, mode: str = "full_packet",
                   remainder: bool = False) -> dict[str, Any]:
    baseline = policy == "step_lockstep"
    strategy = "baseline" if baseline else "swot"
    slow_payload = 3001 if remainder else 4000
    path_tokens = [3] if baseline else []
    groups = [
        {"step_id": 0, "plane_id": 0, "program_epoch_id": 0,
         "configuration_id": 0, "depends_on_token_ids": [0],
         "completion_token_id": 1, "src": 0, "dst": 1, "payload": 1000},
        {"step_id": 0, "plane_id": 1, "program_epoch_id": 0,
         "configuration_id": 0, "depends_on_token_ids": [0],
         "completion_token_id": 2, "src": 2, "dst": 3,
         "payload": slow_payload},
        {"step_id": 1, "plane_id": 0, "program_epoch_id": 1,
         "configuration_id": 1, "depends_on_token_ids": [3],
         "completion_token_id": 4, "src": 0, "dst": 2, "payload": 1000},
        {"step_id": 1, "plane_id": 1, "program_epoch_id": 1,
         "configuration_id": 0, "depends_on_token_ids": [3],
         "completion_token_id": 5, "src": 2, "dst": 3, "payload": 1000},
    ]
    return make_plan(
        f"k2-{policy}-{mode}-remainder-{int(remainder)}", strategy=strategy,
        dependency_mode="global_step_barrier", path_policy=policy,
        execution_mode=mode, node_count=4, plane_count=2,
        configurations=[[1, 0, 3, 2], [2, 3, 0, 1]],
        tokens=[token(0, "collective_start", None),
                token(1, "flow_group_complete", 0),
                token(2, "flow_group_complete", 1), token(3, "step_complete", 0),
                token(4, "flow_group_complete", 2),
                token(5, "flow_group_complete", 3), token(6, "step_complete", 1)],
        steps=[
            {"completion_token_id": 3, "flow_group_ids": [0, 1],
             "phase": "phase0", "step_id": 0},
            {"completion_token_id": 6, "flow_group_ids": [2, 3],
             "phase": "phase1", "step_id": 1},
        ], groups=groups,
        plane_epochs=[
            [
                {"configuration_id": 0, "flow_group_ids": [0],
                 "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
                 "transition": "initial"},
                {"configuration_id": 1, "flow_group_ids": [2],
                 "path_prep_not_before_token_ids": path_tokens,
                 "program_epoch_id": 1, "transition": "reconfigure"},
            ],
            [
                {"configuration_id": 0, "flow_group_ids": [1],
                 "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
                 "transition": "initial"},
                {"configuration_id": 0, "flow_group_ids": [3],
                 "path_prep_not_before_token_ids": path_tokens,
                 "program_epoch_id": 1, "transition": "retain"},
            ],
        ],
    )


def isolation_plan(reconfigure: bool, mode: str = "full_packet") -> dict[str, Any]:
    target_config = 1 if reconfigure else 0
    target_dst = 2 if reconfigure else 1
    transition = "reconfigure" if reconfigure else "retain"
    groups = [
        {"step_id": 0, "plane_id": 0, "program_epoch_id": 0,
         "configuration_id": 0, "depends_on_token_ids": [0],
         "completion_token_id": 1, "src": 0, "dst": 1, "payload": 1000},
        {"step_id": 0, "plane_id": 1, "program_epoch_id": 0,
         "configuration_id": 0, "depends_on_token_ids": [0],
         "completion_token_id": 2, "src": 2, "dst": 3, "payload": 5000},
        {"step_id": 1, "plane_id": 0, "program_epoch_id": 1,
         "configuration_id": target_config, "depends_on_token_ids": [1],
         "completion_token_id": 4, "src": 0, "dst": target_dst,
         "payload": 1000},
    ]
    return make_plan(
        f"isolation-{transition}-{mode}", strategy="swot",
        dependency_mode="explicit_group_dag", path_policy="overlap_earliest",
        execution_mode=mode, node_count=4, plane_count=2,
        configurations=[[1, 0, 3, 2], [2, 3, 0, 1]],
        tokens=[token(0, "collective_start", None),
                token(1, "flow_group_complete", 0),
                token(2, "flow_group_complete", 1), token(3, "step_complete", 0),
                token(4, "flow_group_complete", 2), token(5, "step_complete", 1)],
        steps=[
            {"completion_token_id": 3, "flow_group_ids": [0, 1],
             "phase": "phase0", "step_id": 0},
            {"completion_token_id": 5, "flow_group_ids": [2],
             "phase": "phase1", "step_id": 1},
        ], groups=groups,
        plane_epochs=[
            [
                {"configuration_id": 0, "flow_group_ids": [0],
                 "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
                 "transition": "initial"},
                {"configuration_id": target_config, "flow_group_ids": [2],
                 "path_prep_not_before_token_ids": [], "program_epoch_id": 1,
                 "transition": transition},
            ],
            [{"configuration_id": 0, "flow_group_ids": [1],
              "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
              "transition": "initial"}],
        ],
    )


def ready_reserved_plan(mode: str = "full_packet") -> dict[str, Any]:
    groups = [
        {"step_id": 0, "plane_id": 0, "program_epoch_id": 0,
         "configuration_id": 0, "depends_on_token_ids": [0],
         "completion_token_id": 1, "src": 0, "dst": 1, "payload": 1000},
        {"step_id": 0, "plane_id": 1, "program_epoch_id": 0,
         "configuration_id": 0, "depends_on_token_ids": [0],
         "completion_token_id": 2, "src": 2, "dst": 3, "payload": 4000},
        {"step_id": 1, "plane_id": 0, "program_epoch_id": 1,
         "configuration_id": 1, "depends_on_token_ids": [2],
         "completion_token_id": 4, "src": 0, "dst": 2, "payload": 1000},
        {"step_id": 2, "plane_id": 0, "program_epoch_id": 2,
         "configuration_id": 0, "depends_on_token_ids": [1],
         "completion_token_id": 6, "src": 0, "dst": 1, "payload": 1000},
    ]
    return make_plan(
        f"ready-reserved-{mode}", strategy="swot",
        dependency_mode="explicit_group_dag", path_policy="overlap_earliest",
        execution_mode=mode, node_count=4, plane_count=2,
        configurations=[[1, 0, 3, 2], [2, 3, 0, 1]],
        tokens=[token(0, "collective_start", None),
                token(1, "flow_group_complete", 0),
                token(2, "flow_group_complete", 1), token(3, "step_complete", 0),
                token(4, "flow_group_complete", 2), token(5, "step_complete", 1),
                token(6, "flow_group_complete", 3), token(7, "step_complete", 2)],
        steps=[
            {"completion_token_id": 3, "flow_group_ids": [0, 1],
             "phase": "phase0", "step_id": 0},
            {"completion_token_id": 5, "flow_group_ids": [2],
             "phase": "phase1", "step_id": 1},
            {"completion_token_id": 7, "flow_group_ids": [3],
             "phase": "phase2", "step_id": 2},
        ], groups=groups,
        plane_epochs=[
            [
                {"configuration_id": 0, "flow_group_ids": [0],
                 "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
                 "transition": "initial"},
                {"configuration_id": 1, "flow_group_ids": [2],
                 "path_prep_not_before_token_ids": [], "program_epoch_id": 1,
                 "transition": "reconfigure"},
                {"configuration_id": 0, "flow_group_ids": [3],
                 "path_prep_not_before_token_ids": [], "program_epoch_id": 2,
                 "transition": "reconfigure"},
            ],
            [{"configuration_id": 0, "flow_group_ids": [1],
              "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
              "transition": "initial"}],
        ],
    )


def dual_plane_reconfiguration_plan(
    mode: str = "full_packet",
) -> dict[str, Any]:
    groups = [
        {"step_id": 0, "plane_id": 0, "program_epoch_id": 0,
         "configuration_id": 0, "depends_on_token_ids": [0],
         "completion_token_id": 1, "src": 0, "dst": 1, "payload": 1000},
        {"step_id": 0, "plane_id": 1, "program_epoch_id": 0,
         "configuration_id": 0, "depends_on_token_ids": [0],
         "completion_token_id": 2, "src": 2, "dst": 3, "payload": 4000},
        {"step_id": 1, "plane_id": 0, "program_epoch_id": 1,
         "configuration_id": 1, "depends_on_token_ids": [3],
         "completion_token_id": 4, "src": 0, "dst": 2, "payload": 1501},
        {"step_id": 1, "plane_id": 1, "program_epoch_id": 1,
         "configuration_id": 1, "depends_on_token_ids": [3],
         "completion_token_id": 5, "src": 2, "dst": 0, "payload": 3001},
    ]
    return make_plan(
        f"dual-plane-reconfiguration-{mode}", strategy="baseline",
        dependency_mode="global_step_barrier", path_policy="step_lockstep",
        execution_mode=mode, node_count=4, plane_count=2,
        configurations=[[1, 0, 3, 2], [2, 3, 0, 1]],
        tokens=[token(0, "collective_start", None),
                token(1, "flow_group_complete", 0),
                token(2, "flow_group_complete", 1), token(3, "step_complete", 0),
                token(4, "flow_group_complete", 2),
                token(5, "flow_group_complete", 3), token(6, "step_complete", 1)],
        steps=[
            {"completion_token_id": 3, "flow_group_ids": [0, 1],
             "phase": "phase0", "step_id": 0},
            {"completion_token_id": 6, "flow_group_ids": [2, 3],
             "phase": "phase1", "step_id": 1},
        ], groups=groups,
        plane_epochs=[
            [
                {"configuration_id": 0, "flow_group_ids": [0],
                 "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
                 "transition": "initial"},
                {"configuration_id": 1, "flow_group_ids": [2],
                 "path_prep_not_before_token_ids": [3], "program_epoch_id": 1,
                 "transition": "reconfigure"},
            ],
            [
                {"configuration_id": 0, "flow_group_ids": [1],
                 "path_prep_not_before_token_ids": [], "program_epoch_id": 0,
                 "transition": "initial"},
                {"configuration_id": 1, "flow_group_ids": [3],
                 "path_prep_not_before_token_ids": [3], "program_epoch_id": 1,
                 "transition": "reconfigure"},
            ],
        ],
    )


class Harness:
    def __init__(self, binary: Path, output_dir: Path | None):
        self.binary = binary
        self.output_dir = output_dir
        self.artifacts: list[dict[str, Any]] = []
        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)

    def invoke(
        self,
        plan: dict[str, Any],
        *,
        expected_exit: int = 0,
        fault: str | None = None,
        artifact_name: str | None = None,
    ) -> dict[str, Any]:
        with tempfile.TemporaryDirectory(prefix="phase05-runtime-") as temp_dir:
            plan_path = Path(temp_dir) / "plan.json"
            plan_path.write_bytes(canonical_bytes(plan))
            command = [str(self.binary), "--plan", str(plan_path)]
            if fault is not None:
                command.extend(["--fault", fault])
            result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != expected_exit:
            raise AssertionError(
                f"unexpected exit {result.returncode} != {expected_exit}\n"
                f"stdout={result.stdout}\nstderr={result.stderr}"
            )
        summary = json.loads(result.stdout)
        trace = summary["trace"]
        assert [event["event_index"] for event in trace] == list(range(len(trace)))
        assert [event["time_ps"] for event in trace] == sorted(
            event["time_ps"] for event in trace
        )
        if artifact_name is not None and self.output_dir is not None:
            self._write_artifacts(artifact_name, plan, summary)
        return summary

    def invoke_contract(self, name: str, **kwargs: Any) -> dict[str, Any]:
        plan = json.loads((CONTRACT_PLANS / f"{name}.json").read_text())
        return self.invoke(plan, **kwargs)

    def _write_artifacts(
        self, name: str, plan: dict[str, Any], summary: dict[str, Any]
    ) -> None:
        assert self.output_dir is not None
        case_dir = self.output_dir / name
        case_dir.mkdir(parents=True, exist_ok=True)
        case_dir.joinpath("plan.json").write_bytes(canonical_bytes(plan))
        case_dir.joinpath("summary.json").write_bytes(canonical_bytes(summary))
        trace_bytes = b"".join(canonical_bytes(event) for event in summary["trace"])
        case_dir.joinpath("operations.jsonl").write_bytes(trace_bytes)
        expected_per_rank_sent = [0] * plan["topology"]["node_count"]
        expected_per_rank_received = [0] * plan["topology"]["node_count"]
        expected_per_plane = [0] * plan["topology"]["plane_count"]
        for flow in plan["flows"]:
            payload = flow["payload_bytes"]
            expected_per_rank_sent[flow["src_rank"]] += payload
            expected_per_rank_received[flow["dst_rank"]] += payload
            group = plan["flow_groups"][flow["flow_group_id"]]
            expected_per_plane[group["plane_id"]] += payload
        manifest = {
            "case_id": plan["case_id"],
            "dependency_mode": plan["dependency_mode"],
            "execution_mode": plan["execution_mode"],
            "expected_flow_count": len(plan["flows"]),
            "expected_flow_group_count": len(plan["flow_groups"]),
            "expected_payload_bytes": sum(flow["payload_bytes"] for flow in plan["flows"]),
            "expected_per_plane_payload_bytes": expected_per_plane,
            "expected_per_rank_received_payload_bytes": expected_per_rank_received,
            "expected_per_rank_sent_payload_bytes": expected_per_rank_sent,
            "flow_manifest": [
                {
                    "dst_rank": flow["dst_rank"],
                    "flow_group_id": flow["flow_group_id"],
                    "flow_id": flow["flow_id"],
                    "payload_bytes": flow["payload_bytes"],
                    "src_rank": flow["src_rank"],
                }
                for flow in plan["flows"]
            ],
            "flow_group_manifest": [
                {
                    "configuration_id": group["configuration_id"],
                    "depends_on_token_ids": group["depends_on_token_ids"],
                    "flow_group_id": group["flow_group_id"],
                    "flow_ids": group["flow_ids"],
                    "plane_id": group["plane_id"],
                    "program_epoch_id": group["program_epoch_id"],
                    "step_id": group["step_id"],
                }
                for group in plan["flow_groups"]
            ],
            "observed_traffic": summary["traffic"],
            "operations_file_sha256": hashlib.sha256(trace_bytes).hexdigest(),
            "path_preparation_policy": plan["path_preparation_policy"],
            "plane_programs": plan["plane_programs"],
            "plan_file_sha256": hashlib.sha256(canonical_bytes(plan)).hexdigest(),
            "status": summary["status"],
            "stop_reason": summary["stop_reason"],
            "summary_file_sha256": hashlib.sha256(canonical_bytes(summary)).hexdigest(),
        }
        case_dir.joinpath("traffic-manifest.json").write_bytes(canonical_bytes(manifest))
        self.artifacts.append({
            "artifact_name": name,
            "case_id": plan["case_id"],
            "operations_file_sha256": manifest["operations_file_sha256"],
            "plan_file_sha256": manifest["plan_file_sha256"],
            "relative_path": name,
            "status": summary["status"],
            "stop_reason": summary["stop_reason"],
            "summary_file_sha256": manifest["summary_file_sha256"],
        })

    def write_delivery_manifest(self) -> None:
        if self.output_dir is None:
            return
        document = {
            "artifact_case_count": len(self.artifacts),
            "cases": self.artifacts,
            "schema_version": "phase05-delivery-manifest/v1",
        }
        self.output_dir.joinpath("delivery-manifest.json").write_bytes(
            canonical_bytes(document)
        )


def assert_success(summary: dict[str, Any]) -> None:
    assert summary["status"] == "success", summary
    assert summary["stop_reason"] == "collective_complete"
    assert summary["blocked_state"] is None
    traffic = summary["traffic"]
    assert traffic["expected_flow_count"] == traffic["completed_flow_count"]
    assert traffic["expected_flow_group_count"] == traffic["completed_flow_group_count"]
    assert traffic["expected_payload_bytes"] == traffic["sent_payload_bytes"]
    assert traffic["expected_payload_bytes"] == traffic["received_payload_bytes"]
    assert traffic["in_flight_transit_unit_count"] == 0
    assert all(flow["complete"] for flow in summary["flows"])
    assert all(plane["state"] == "program_complete" for plane in summary["planes"])


def semantic_projection(summary: dict[str, Any]) -> dict[str, Any]:
    projected = copy.deepcopy(summary)
    projected.pop("execution_mode")
    projected["traffic"].pop("processed_event_count")
    projected["traffic"].pop("simulated_transit_unit_count")
    return projected


def run_epoch_suite(harness: Harness) -> int:
    initial = harness.invoke_contract("one_plane_one_group",
                                      artifact_name="initial-generation-zero")
    assert_success(initial)
    assert initial["planes"][0]["epochs"][0]["physical_config_generation"] == 0

    retain = harness.invoke_contract("retain_new_epoch_same_generation",
                                     artifact_name="retain-generation-stable")
    assert_success(retain)
    assert [epoch["physical_config_generation"] for epoch in retain["planes"][0]["epochs"]] == [0, 0]
    assert retain["planes"][0]["epochs"][1]["program_epoch_id"] == 1
    assert retain["planes"][0]["epochs"][1]["reconfiguration_start_ps"] is None

    plan = three_epoch_plan()
    reconfigured = harness.invoke(plan, artifact_name="epoch-generation-full")
    assert_success(reconfigured)
    epochs = reconfigured["planes"][0]["epochs"]
    assert [epoch["physical_config_generation"] for epoch in epochs] == [0, 1, 2]
    assert [epoch["program_epoch_id"] for epoch in epochs] == [0, 1, 2]
    for epoch in epochs[1:]:
        assert epoch["reconfiguration_end_ps"] - epoch["reconfiguration_start_ps"] == 200
        assert epoch["path_prep_start_ps"] >= epochs[epoch["program_epoch_id"] - 1]["close_ps"]
    assert epochs[1]["reconfiguration_start_ps"] == reconfigured["flows"][0]["last_payload_received_ps"]
    assert epochs[1]["reconfiguration_start_ps"] > reconfigured["flows"][0]["last_payload_sent_ps"]

    dual = harness.invoke(dual_plane_reconfiguration_plan(),
                          artifact_name="dual-plane-reconfiguration")
    assert_success(dual)
    dual_windows = [plane["epochs"][1] for plane in dual["planes"]]
    assert dual_windows[0]["reconfiguration_start_ps"] == dual_windows[1]["reconfiguration_start_ps"]
    assert dual_windows[0]["reconfiguration_end_ps"] == dual_windows[1]["reconfiguration_end_ps"]
    assert all(window["reconfiguration_end_ps"] - window["reconfiguration_start_ps"] == 200
               for window in dual_windows)
    assert dual["flow_groups"][2]["release_ps"] >= dual_windows[0]["reconfiguration_end_ps"]
    assert dual["flow_groups"][3]["release_ps"] >= dual_windows[1]["reconfiguration_end_ps"]

    isolation = harness.invoke(isolation_plan(True), artifact_name="plane-isolation-reconfigure")
    control = harness.invoke(isolation_plan(False), artifact_name="plane-isolation-control")
    assert_success(isolation)
    assert_success(control)
    for field in ("release_ps", "last_payload_sent_ps", "last_payload_received_ps"):
        assert isolation["flows"][1][field] == control["flows"][1][field]
    p0_window = isolation["planes"][0]["epochs"][1]
    assert isolation["flows"][1]["last_payload_sent_ps"] > p0_window["reconfiguration_start_ps"]

    exact_plan = three_epoch_plan("exact_coalesced")
    exact = harness.invoke(exact_plan, artifact_name="epoch-generation-exact")
    assert semantic_projection(reconfigured) == semantic_projection(exact)
    return 6


def run_dependency_suite(harness: Harness) -> int:
    independent = harness.invoke_contract(
        "two_groups_same_epoch_independent_release",
        artifact_name="same-epoch-independent-groups")
    assert_success(independent)
    groups = {group["flow_group_id"]: group for group in independent["flow_groups"]}
    assert groups[0]["release_ps"] == 0
    assert groups[1]["release_ps"] == 0
    assert groups[2]["release_ps"] == groups[0]["completion_ps"]
    assert groups[2]["release_ps"] < groups[1]["completion_ps"]

    barrier = harness.invoke(global_k2_plan("overlap_earliest"),
                             artifact_name="global-barrier")
    assert_success(barrier)
    step0_complete = barrier["steps"][0]["completion_ps"]
    assert barrier["flow_groups"][2]["release_ps"] == step0_complete
    assert barrier["flow_groups"][3]["release_ps"] == step0_complete
    assert step0_complete == barrier["flow_groups"][1]["completion_ps"]

    dag = harness.invoke(isolation_plan(True), artifact_name="explicit-group-dag")
    assert_success(dag)
    assert dag["flow_groups"][2]["release_ps"] == dag["flow_groups"][0]["completion_ps"] + 200
    assert dag["flow_groups"][2]["release_ps"] < dag["flow_groups"][1]["completion_ps"]

    base = json.loads((CONTRACT_PLANS / "one_plane_one_group.json").read_text())
    missing = harness.invoke(base, expected_exit=3,
                             fault="suppress-flow-completion=0",
                             artifact_name="missing-callback-deadlock")
    assert missing["stop_reason"] == "deadlock"
    assert missing["error_code"] == "no_progress"
    assert missing["blocked_state"]["unfinished_flow_group_ids"] == [0]

    duplicate = harness.invoke(base, expected_exit=3,
                               fault="duplicate-flow-completion=0",
                               artifact_name="duplicate-callback-invariant")
    assert duplicate["stop_reason"] == "invariant_violation"
    assert duplicate["error_code"] == "dependency_violation"
    return 5


def run_path_policy_suite(harness: Harness) -> int:
    overlap = harness.invoke(global_k2_plan("overlap_earliest"),
                             artifact_name="overlap-earliest-delayed")
    lockstep = harness.invoke(global_k2_plan("step_lockstep"),
                              artifact_name="step-lockstep-delayed")
    assert_success(overlap)
    assert_success(lockstep)
    overlap_p0 = overlap["planes"][0]["epochs"][1]
    lockstep_p0 = lockstep["planes"][0]["epochs"][1]
    assert overlap_p0["reconfiguration_start_ps"] < overlap["steps"][0]["completion_ps"]
    assert lockstep_p0["reconfiguration_start_ps"] == lockstep["steps"][0]["completion_ps"]
    assert lockstep["flow_groups"][3]["release_ps"] == lockstep_p0["path_ready_ps"]
    assert lockstep["flow_groups"][2]["release_ps"] == lockstep["flow_groups"][3]["release_ps"]
    assert overlap["collective_complete_ps"] < lockstep["collective_complete_ps"]

    remainder = harness.invoke(global_k2_plan("step_lockstep", remainder=True),
                               artifact_name="step-lockstep-remainder")
    assert_success(remainder)
    assert remainder["flows"][1]["tail_payload_bytes"] == 1
    assert remainder["flow_groups"][2]["release_ps"] == remainder["flow_groups"][3]["release_ps"]

    static = harness.invoke_contract("one_shot_static_preinstalled",
                                     artifact_name="static-preinstalled")
    assert_success(static)
    assert all(len(plane["epochs"]) <= 1 for plane in static["planes"])
    assert all(epoch["physical_config_generation"] == 0
               for plane in static["planes"] for epoch in plane["epochs"])
    assert not any(event["event_type"].startswith("reconfiguration")
                   for event in static["trace"])

    reserved = harness.invoke(ready_reserved_plan(), artifact_name="ready-reserved")
    assert_success(reserved)
    p0_epochs = reserved["planes"][0]["epochs"]
    assert p0_epochs[1]["path_ready_ps"] < reserved["flow_groups"][2]["release_ps"]
    assert p0_epochs[2]["path_prep_start_ps"] >= p0_epochs[1]["close_ps"]
    assert p0_epochs[2]["path_prep_start_ps"] > p0_epochs[1]["path_ready_ps"]

    for builder, name in ((lambda mode: global_k2_plan("overlap_earliest", mode), "global"),
                          (ready_reserved_plan, "dag")):
        full = harness.invoke(builder("full_packet"),
                              artifact_name=f"equivalence-{name}-full")
        exact = harness.invoke(builder("exact_coalesced"),
                               artifact_name=f"equivalence-{name}-exact")
        assert semantic_projection(full) == semantic_projection(exact), name
    return 6


def run_watchdog_suite(harness: Harness) -> int:
    base = json.loads((CONTRACT_PLANS / "one_plane_one_group.json").read_text())
    expected_cct = 20_020
    for limit, expected_exit, expected_reason in (
        (expected_cct - 1, 3, "max_simulation_time"),
        (expected_cct, 0, "collective_complete"),
        (expected_cct + 1, 0, "collective_complete"),
    ):
        plan = copy.deepcopy(base)
        plan["run_limits"]["max_sim_time_ps"] = limit
        summary = harness.invoke(plan, expected_exit=expected_exit,
                                 artifact_name=f"max-time-{limit}")
        assert summary["stop_reason"] == expected_reason
        if expected_exit == 3:
            assert summary["blocked_state"] is not None

    one_event = copy.deepcopy(base)
    one_event["run_limits"]["max_events"] = 1
    limited = harness.invoke(one_event, expected_exit=3,
                             artifact_name="max-events-1")
    assert limited["stop_reason"] == "max_event_count"
    assert limited["traffic"]["processed_event_count"] == 1
    assert limited["traffic"]["in_flight_transit_unit_count"] == 1

    two_events = copy.deepcopy(base)
    two_events["run_limits"]["max_events"] = 2
    completed = harness.invoke(two_events, artifact_name="max-events-2")
    assert_success(completed)
    assert completed["traffic"]["processed_event_count"] == 2

    reference: bytes | None = None
    plan_bytes = canonical_bytes(base)
    with tempfile.TemporaryDirectory(prefix="phase05-determinism-") as temp_dir:
        plan_path = Path(temp_dir) / "plan.json"
        plan_path.write_bytes(plan_bytes)
        for _ in range(100):
            result = subprocess.run(
                [str(harness.binary), "--plan", str(plan_path)],
                capture_output=True,
            )
            assert result.returncode == 0, result.stderr
            if reference is None:
                reference = result.stdout
            else:
                assert result.stdout == reference
    return 6


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument(
        "--suite",
        choices=("epoch", "dependencies", "path-policies", "watchdog", "all"),
        required=True,
    )
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    harness = Harness(args.binary.resolve(), args.output_dir)
    suites = {
        "epoch": run_epoch_suite,
        "dependencies": run_dependency_suite,
        "path-policies": run_path_policy_suite,
        "watchdog": run_watchdog_suite,
    }
    selected = suites.items() if args.suite == "all" else [(args.suite, suites[args.suite])]
    total = 0
    for name, suite in selected:
        count = suite(harness)
        total += count
        print(f"phase05 {name}: PASS ({count} audited cases)")
    harness.write_delivery_manifest()
    print(f"phase05 runtime total: PASS ({total} audited cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
