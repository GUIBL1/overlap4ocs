#!/usr/bin/env python3
"""Phase 06 static semantic-fixture and full/coalesced acceptance."""

from __future__ import annotations

import argparse
import copy
from pathlib import Path

from jsonschema import Draft202012Validator

from phase06_acceptance import (
    DEFAULT_BINARY,
    FIXTURE_ROOT,
    assert_manifest_paths,
    capabilities,
    load_json,
    load_manifests,
    semantic_equivalence_projection,
    validate_one,
)


REQUIRED = {
    "one_flow_exact_tail",
    "sparse_p4",
    "static_k2_isolation",
    "k2_reconfiguration_overlap",
    "global_step_barrier",
    "explicit_group_dag",
    "swot_overlap_earliest",
    "baseline_step_lockstep",
    "one_shot_static_preinstalled",
    "retain_and_bypass",
    "paper_fig5_swot",
    "paper_fig5_strawman",
    "failure_state_projection",
}


def assert_plan_mode_pair(directory: Path, manifest: dict) -> None:
    full = load_json(directory / manifest["plans"]["full_packet"])
    exact = load_json(directory / manifest["plans"]["exact_coalesced"])
    full.pop("execution_mode")
    exact.pop("execution_mode")
    assert full == exact, f"plans differ beyond execution_mode: {manifest['fixture_id']}"


def assert_case_specific(fixture_id: str, full: dict, exact: dict) -> None:
    result = exact
    if fixture_id == "paper_fig5_swot":
        assert result["timing"]["simulated_cct_ps"] == 1_200_000_000
        sequences = [
            [epoch["configuration_id"] for epoch in plane["epochs"]]
            for plane in result["planes"]
        ]
        assert sequences == [[0, 2, 2, 0], [0, 1, 1, 0]]
        assert any(epoch["transition"] == "retain" for plane in result["planes"] for epoch in plane["epochs"])
        step6_groups = [group for group in result["flow_groups"] if group["step_id"] == 5]
        assert all(group["dependency_ready_ps"] == result["steps"][4]["completion_ps"] for group in step6_groups)
        assert all(group["release_ps"] >= group["dependency_ready_ps"] for group in step6_groups)
    elif fixture_id == "paper_fig5_strawman":
        assert result["timing"]["simulated_cct_ps"] == 1_500_000_000
        assert all(
            [epoch["configuration_id"] for epoch in plane["epochs"]] == [0, 1, 2, 2, 1, 0]
            for plane in result["planes"]
        )
        for step in result["steps"]:
            releases = {
                result["flow_groups"][group_id]["release_ps"]
                for group_id in step["flow_group_ids"]
            }
            assert len(releases) == 1
    elif fixture_id == "swot_overlap_earliest":
        assert any(
            epoch["program_epoch_id"] > 0
            and epoch["path_prep_start_ps"] < result["steps"][0]["completion_ps"]
            for plane in result["planes"] for epoch in plane["epochs"]
            if epoch["path_prep_start_ps"] is not None
        )
    elif fixture_id == "baseline_step_lockstep":
        for plane in result["planes"]:
            for epoch in plane["epochs"][1:]:
                assert epoch["path_prep_start_ps"] >= result["steps"][epoch["program_epoch_id"] - 1]["completion_ps"]
    elif fixture_id == "one_shot_static_preinstalled":
        assert all(len(plane["epochs"]) <= 1 for plane in result["planes"])
        assert all(epoch["transition"] == "initial" for plane in result["planes"] for epoch in plane["epochs"])
    elif fixture_id == "retain_and_bypass":
        generations = [epoch["physical_config_generation"] for epoch in result["planes"][0]["epochs"]]
        assert generations == [0, 0]
    elif fixture_id == "failure_state_projection":
        assert result["status"] == "failure"
        assert result["stop_reason"] == "max_event_count"
        assert result["blocked_state"]["unfinished_flow_ids"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    args = parser.parse_args()
    binary = args.binary.resolve()
    manifests = load_manifests("runtime")
    observed = {manifest["fixture_id"] for _, manifest in manifests}
    assert observed == REQUIRED, (observed, REQUIRED)
    projection_validator = Draft202012Validator(
        load_json(FIXTURE_ROOT / "expected-result-projection-v1.schema.json")
    )
    caps = capabilities(binary)
    for directory, manifest in manifests:
        assert_manifest_paths(directory, manifest)
        assert_plan_mode_pair(directory, manifest)
        results = {}
        for mode in ("full_packet", "exact_coalesced"):
            results[mode], _, _ = validate_one(
                binary, directory, manifest, mode, caps, projection_validator
            )
        if manifest["fixture_id"] != "failure_state_projection":
            assert semantic_equivalence_projection(results["full_packet"]) == semantic_equivalence_projection(results["exact_coalesced"]), manifest["fixture_id"]
        assert_case_specific(
            manifest["fixture_id"], results["full_packet"], results["exact_coalesced"]
        )
    print(f"phase06 standalone semantic fixtures: PASS ({len(manifests)} fixtures, {len(manifests) * 2} plans)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
