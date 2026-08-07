#!/usr/bin/env python3
"""Phase 06 exact-coalesced scalability and resource-budget gates."""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import tempfile
import time
from pathlib import Path

from jsonschema import Draft202012Validator

from phase06_acceptance import (
    DEFAULT_BINARY,
    FIXTURE_ROOT,
    assert_manifest_paths,
    canonical,
    capabilities,
    load_json,
    load_manifests,
    validate_one,
)


REQUIRED = {
    "large_payload_p2_k1_5gib",
    "scale_p256_k8_512mb_per_rank",
    "program_epochs_32",
}
WALL_BUDGET_SECONDS = 60.0
RSS_BUDGET_KIB = 1024 * 1024


def timed_run(binary: Path, plan: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="phase06-performance-") as directory:
        root = Path(directory)
        result = root / "result.json"
        time_output = root / "time.txt"
        command = [
            "/usr/bin/time", "-v", "-o", str(time_output),
            str(binary), "run", "--plan", str(plan), "--result", str(result),
        ]
        started = time.monotonic_ns()
        completed = subprocess.run(command, check=False, capture_output=True)
        elapsed_ns = time.monotonic_ns() - started
        assert completed.returncode == 0, completed.stderr
        document = json.loads(result.read_text())
        metrics = {}
        for line in time_output.read_text().splitlines():
            if ":" not in line:
                continue
            key, value = line.strip().split(":", 1)
            metrics[key] = value.strip()
        rss_kib = int(metrics["Maximum resident set size (kbytes)"])
        wall_seconds = elapsed_ns / 1_000_000_000
        assert wall_seconds <= WALL_BUDGET_SECONDS, wall_seconds
        assert rss_kib <= RSS_BUDGET_KIB, rss_kib
        return {
            "peak_rss_kib": rss_kib,
            "processed_event_count": document["traffic"]["processed_event_count"],
            "simulated_cct_ps": document["timing"]["simulated_cct_ps"],
            "simulated_transit_unit_count": document["traffic"]["simulated_transit_unit_count"],
            "wall_time_ns": elapsed_ns,
        }


def machine() -> dict:
    cpu_model = "unknown"
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text(errors="replace").splitlines():
            if line.startswith("model name") and ":" in line:
                cpu_model = line.split(":", 1)[1].strip()
                break
    return {
        "cpu_count": os.cpu_count(),
        "cpu_model": cpu_model,
        "machine": platform.machine(),
        "platform": platform.platform(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--write-evidence", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    manifests = load_manifests("performance")
    assert {manifest["fixture_id"] for _, manifest in manifests} == REQUIRED
    caps = capabilities(binary)
    projection_validator = Draft202012Validator(
        load_json(FIXTURE_ROOT / "expected-result-projection-v1.schema.json")
    )
    evidence = {
        "budget": {"peak_rss_kib": RSS_BUDGET_KIB, "wall_time_ns": int(WALL_BUDGET_SECONDS * 1_000_000_000)},
        "cases": [],
        "machine": machine(),
        "schema_version": "phase06-performance-evidence/v1",
    }
    for directory, manifest in manifests:
        assert_manifest_paths(directory, manifest)
        assert manifest["plans"]["full_packet"] is None
        assert manifest["expected_result_projections"]["full_packet"] is None
        assert manifest["expected_traces"]["full_packet"] is None
        assert manifest["mode_expectations"]["full_packet"] is None
        result, _, _ = validate_one(
            binary, directory, manifest, "exact_coalesced", caps,
            projection_validator,
        )
        fixture_id = manifest["fixture_id"]
        inventory = manifest["expected_inventory"]
        if fixture_id == "large_payload_p2_k1_5gib":
            assert inventory["payload_bytes"] == 5 * 1024**3
            assert result["traffic"]["logical_packet_count"] > 3_000_000
        elif fixture_id == "scale_p256_k8_512mb_per_rank":
            plan = load_json(directory / manifest["plans"]["exact_coalesced"])
            assert plan["topology"]["node_count"] == 256
            assert plan["topology"]["plane_count"] == 8
            assert inventory["per_rank_sent_payload_bytes"] == [512_000_000] * 256
            assert inventory["flow_count"] == 2048
        elif fixture_id == "program_epochs_32":
            transitions = [epoch["transition"] for epoch in inventory["plane_epochs"]]
            assert len(transitions) >= 32
            assert "retain" in transitions and "reconfigure" in transitions
        measured = timed_run(binary, directory / manifest["plans"]["exact_coalesced"])
        limit = manifest["mode_expectations"]["exact_coalesced"]
        assert measured["processed_event_count"] <= limit["max_processed_events"]
        assert measured["simulated_transit_unit_count"] <= limit["max_transit_units"]
        evidence["cases"].append({"fixture_id": fixture_id, **measured})
    if args.write_evidence is not None:
        args.write_evidence.parent.mkdir(parents=True, exist_ok=True)
        args.write_evidence.write_bytes(canonical(evidence))
    print("phase06 performance gates: PASS (3 cases; each <60 s and <1 GiB RSS)")
    for row in evidence["cases"]:
        print(f"  {row['fixture_id']}: {row['wall_time_ns'] / 1e9:.3f}s, {row['peak_rss_kib']} KiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
