"""Shared readers and assertions for the static Phase 06 acceptance corpus."""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator


TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = TEST_DIR.parents[2]
FIXTURE_ROOT = TEST_DIR / "fixtures/v2"
DEFAULT_BINARY = TEST_DIR.parent / "build/htsim_ocs"

sys.path.insert(0, str(REPO_ROOT))
from simulator.contracts.validation import (  # noqa: E402
    validate_execution_plan,
    validate_operation_event,
    validate_simulation_result,
)


STABLE_PROVENANCE = (
    "htsim_upstream_git_sha",
    "plan_schema_id",
    "plan_schema_sha256",
    "result_schema_id",
    "result_schema_sha256",
    "exact_coalesced_equivalence_version",
)


def canonical(document: Any) -> bytes:
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode()


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text())


def project_result(fixture_id: str, result: dict[str, Any]) -> dict[str, Any]:
    core = copy.deepcopy(result)
    provenance = core.pop("provenance")
    return {
        "execution_mode": result["execution_mode"],
        "fixture_id": fixture_id,
        "result_core": core,
        "schema_version": "htsim-ocs-expected-result-projection/v1",
        "stable_provenance": {key: provenance[key] for key in STABLE_PROVENANCE},
    }


def audit_inventory(plan: dict[str, Any]) -> dict[str, Any]:
    mtu = plan["transport"]["mtu_bytes"]
    groups = {group["flow_group_id"]: group for group in plan["flow_groups"]}
    rank_sent = [0] * plan["topology"]["node_count"]
    rank_received = [0] * plan["topology"]["node_count"]
    plane_bytes = [0] * plan["topology"]["plane_count"]
    flows = []
    for flow in plan["flows"]:
        payload = flow["payload_bytes"]
        rank_sent[flow["src_rank"]] += payload
        rank_received[flow["dst_rank"]] += payload
        plane_bytes[groups[flow["flow_group_id"]]["plane_id"]] += payload
        flows.append({
            "dst_rank": flow["dst_rank"],
            "flow_group_id": flow["flow_group_id"],
            "flow_id": flow["flow_id"],
            "last_packet_payload_bytes": ((payload - 1) % mtu) + 1,
            "logical_packet_count": (payload + mtu - 1) // mtu,
            "payload_bytes": payload,
            "src_rank": flow["src_rank"],
        })
    edges = [
        {"consumer_flow_group_id": group["flow_group_id"], "token_id": token_id}
        for group in plan["flow_groups"]
        for token_id in group["depends_on_token_ids"]
    ]
    return {
        "dependency_edges": sorted(edges, key=lambda row: (row["token_id"], row["consumer_flow_group_id"])),
        "flow_count": len(flows),
        "flow_group_count": len(plan["flow_groups"]),
        "flow_groups": [{key: group[key] for key in (
            "configuration_id", "depends_on_token_ids", "flow_group_id",
            "flow_ids", "plane_id", "program_epoch_id", "step_id")}
            for group in plan["flow_groups"]],
        "flows": flows,
        "payload_bytes": sum(flow["payload_bytes"] for flow in flows),
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


def load_manifests(category: str) -> list[tuple[Path, dict[str, Any]]]:
    schema = load_json(FIXTURE_ROOT / "fixture-manifest-v1.schema.json")
    validator = Draft202012Validator(schema)
    result = []
    for path in sorted((FIXTURE_ROOT / category).glob("*/manifest.json")):
        manifest = load_json(path)
        validator.validate(manifest)
        result.append((path.parent, manifest))
    return result


def capabilities(binary: Path) -> dict[str, Any]:
    completed = subprocess.run(
        [str(binary), "capabilities", "--json"], check=True,
        capture_output=True, text=True,
    )
    return json.loads(completed.stdout)


def assert_build_provenance(result: dict[str, Any], caps: dict[str, Any]) -> None:
    provenance = result["provenance"]
    build = caps["build"]
    assert provenance["htsim_ocs_version"] == caps["htsim_ocs_version"]
    assert provenance["htsim_upstream_git_sha"] == build["htsim_upstream_git_sha"]
    assert provenance["htsim_local_patchset_sha256"] == build["htsim_local_patchset_sha256"]
    assert provenance["parent_build_commit"] == build["parent_build_commit"]
    assert provenance["compiler_id"] == build["compiler_id"]
    assert provenance["build_flags_sha256"] == build["build_flags_sha256"]


def invoke(
    binary: Path, plan_path: Path, *, request_trace: bool,
) -> tuple[subprocess.CompletedProcess[bytes], bytes, bytes | None]:
    with tempfile.TemporaryDirectory(prefix="phase06-standalone-") as directory:
        root = Path(directory)
        result_path = root / "result.json"
        trace_path = root / "operations.jsonl"
        command = [str(binary), "run", "--plan", str(plan_path),
                   "--result", str(result_path)]
        if request_trace:
            command.extend(["--trace", str(trace_path)])
        completed = subprocess.run(command, check=False, capture_output=True)
        if not result_path.exists():
            raise AssertionError(
                f"missing formal result: rc={completed.returncode} stderr={completed.stderr!r}"
            )
        result_raw = result_path.read_bytes()
        trace_raw = trace_path.read_bytes() if request_trace else None
    return completed, result_raw, trace_raw


def assert_trace(raw: bytes) -> list[dict[str, Any]]:
    assert raw.endswith(b"\n") or raw == b""
    events = [json.loads(line) for line in raw.splitlines()]
    for event in events:
        validate_operation_event(event)
    assert [event["event_index"] for event in events] == list(range(len(events)))
    assert [event["time_ps"] for event in events] == sorted(event["time_ps"] for event in events)
    return events


def semantic_equivalence_projection(result: dict[str, Any]) -> dict[str, Any]:
    value = copy.deepcopy(result)
    value.pop("plan_file_sha256")
    value.pop("execution_mode")
    value.pop("provenance")
    value["traffic"].pop("processed_event_count")
    value["traffic"].pop("simulated_transit_unit_count")
    return value


def assert_serialization_and_latency(plan: dict[str, Any], result: dict[str, Any]) -> None:
    rate = plan["topology"]["per_plane_bps"]
    latency = plan["topology"]["data_latency_ps"]
    for flow in result["flows"]:
        if flow["status"] != "complete":
            continue
        minimum = (flow["payload_bytes"] * 8 * 10**12 + rate - 1) // rate
        assert flow["last_payload_sent_ps"] - flow["release_ps"] >= minimum
        assert flow["last_payload_received_ps"] - flow["last_payload_sent_ps"] == latency


def assert_manifest_paths(directory: Path, manifest: dict[str, Any]) -> None:
    for section in ("plans", "expected_result_projections", "expected_traces"):
        for relative in manifest[section].values():
            if relative is None:
                continue
            path = Path(relative)
            assert not path.is_absolute() and ".." not in path.parts
            assert (directory / path).is_file()
    readme = (directory / "README.md").read_bytes()
    assert hashlib.sha256(readme).hexdigest() == manifest["notes_sha256"]


def validate_one(
    binary: Path, directory: Path, manifest: dict[str, Any], mode: str,
    caps: dict[str, Any], projection_validator: Draft202012Validator,
) -> tuple[dict[str, Any], bytes, bytes | None]:
    plan_name = manifest["plans"][mode]
    assert plan_name is not None
    plan_path = directory / plan_name
    plan_raw = plan_path.read_bytes()
    plan = json.loads(plan_raw)
    validate_execution_plan(plan)
    assert plan["execution_mode"] == mode
    assert audit_inventory(plan) == manifest["expected_inventory"]

    checked = subprocess.run(
        [str(binary), "validate", "--plan", str(plan_path)],
        check=False, capture_output=True,
    )
    assert checked.returncode == 0, checked.stderr
    assert hashlib.sha256(plan_raw).hexdigest().encode() in checked.stdout

    completed, result_raw, trace_raw = invoke(
        binary, plan_path, request_trace=manifest["trace_required"]
    )
    result = json.loads(result_raw)
    validate_simulation_result(result)
    expectation = manifest["mode_expectations"][mode]
    assert expectation is not None
    expected_exit = 0 if expectation["expected_status"] == "success" else 4
    assert completed.returncode == expected_exit, completed.stderr
    assert result["status"] == expectation["expected_status"]
    assert result["timing"]["simulated_cct_ps"] == expectation["expected_cct_ps"]
    assert result["plan_file_sha256"] == hashlib.sha256(plan_raw).hexdigest()
    assert_build_provenance(result, caps)

    expected_projection_path = directory / manifest["expected_result_projections"][mode]
    actual_projection = project_result(manifest["fixture_id"], result)
    projection_validator.validate(actual_projection)
    assert canonical(actual_projection) == expected_projection_path.read_bytes()

    traffic = result["traffic"]
    inventory = manifest["expected_inventory"]
    assert traffic["expected_flow_count"] == inventory["flow_count"]
    assert traffic["expected_flow_group_count"] == inventory["flow_group_count"]
    assert traffic["expected_payload_bytes"] == inventory["payload_bytes"]
    assert traffic["per_plane_payload_bytes"] == inventory["per_plane_payload_bytes"]
    assert traffic["processed_event_count"] <= expectation["max_processed_events"]
    assert traffic["simulated_transit_unit_count"] <= expectation["max_transit_units"]
    if result["status"] == "success":
        assert traffic["sent_payload_bytes"] == inventory["payload_bytes"]
        assert traffic["received_payload_bytes"] == inventory["payload_bytes"]
        assert traffic["per_rank_sent_payload_bytes"] == inventory["per_rank_sent_payload_bytes"]
        assert traffic["per_rank_received_payload_bytes"] == inventory["per_rank_received_payload_bytes"]
        assert traffic["drops"] == traffic["duplicate_payload_bytes"] == traffic["missing_payload_bytes"] == 0
        assert traffic["in_flight_transit_unit_count"] == 0
        assert_serialization_and_latency(plan, result)
    else:
        assert result["timing"]["simulated_cct_ps"] is None
        assert result["blocked_state"] is not None

    if manifest["trace_required"]:
        assert trace_raw is not None
        assert_trace(trace_raw)
        expected_trace = directory / manifest["expected_traces"][mode]
        assert trace_raw == expected_trace.read_bytes()
    else:
        assert trace_raw is None
    return result, result_raw, trace_raw
