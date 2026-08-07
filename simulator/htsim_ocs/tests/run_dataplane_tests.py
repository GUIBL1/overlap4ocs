#!/usr/bin/env python3
"""Phase 04 static data-plane and exact-coalesced equivalence tests."""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
import tempfile
from copy import deepcopy
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
BUILDER_PATH = REPO_ROOT / "tests/fixtures/contracts/v2/build_fixtures.py"
sys.path.insert(0, str(REPO_ROOT))


def load_fixture_builder():
    spec = importlib.util.spec_from_file_location("phase02_fixture_builder", BUILDER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load Phase 02 fixture builder")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BUILDER = load_fixture_builder()


def make_plan(
    name: str,
    *,
    mode: str,
    node_count: int,
    plane_count: int,
    configurations: list[list[int]],
    groups: list[dict[str, int | list[int]]],
    programs: list[list[dict[str, Any]]],
    mtu: int = 1500,
    rate: int = 400_000_000_000,
    latency: int = 20,
) -> dict[str, Any]:
    plan = BUILDER.make_plan(
        name,
        strategy="swot",
        dependency_mode="global_step_barrier",
        node_count=node_count,
        plane_count=plane_count,
        configurations=configurations,
        group_specs=groups,
        program_specs=programs,
        mtu_bytes=mtu,
    )
    plan["execution_mode"] = mode
    plan["topology"]["per_plane_bps"] = rate
    plan["topology"]["data_latency_ps"] = latency
    BUILDER.validate_execution_plan(plan)
    return plan


def single_flow_plan(
    name: str,
    payload: int,
    mode: str,
    *,
    mtu: int = 1500,
    rate: int = 400_000_000_000,
    latency: int = 20,
) -> dict[str, Any]:
    return make_plan(
        name,
        mode=mode,
        node_count=2,
        plane_count=1,
        configurations=[[1, 0]],
        groups=[
            {
                "step_id": 0,
                "plane_id": 0,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": 0,
                "dst_rank": 1,
                "payload_bytes": payload,
            }
        ],
        programs=[
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [0],
                }
            ]
        ],
        mtu=mtu,
        rate=rate,
        latency=latency,
    )


class Harness:
    def __init__(self, binary: Path) -> None:
        self.binary = binary.resolve()
        self.tmp = tempfile.TemporaryDirectory(prefix="htsim-ocs-phase04-")
        self.root = Path(self.tmp.name)
        self.counter = 0

    def close(self) -> None:
        self.tmp.cleanup()

    def write(self, plan: dict[str, Any]) -> Path:
        self.counter += 1
        path = self.root / f"plan-{self.counter:04d}.json"
        path.write_bytes(BUILDER.canonical_json_bytes(plan))
        return path

    def invoke(
        self,
        plan: dict[str, Any],
        mode: str,
        *,
        preflight_only: bool = False,
        expect_exit: int = 0,
        expected_error: str | None = None,
    ) -> dict[str, Any] | None:
        path = self.write(plan)
        argv = [
            str(self.binary),
            "--plan",
            str(path),
            "--execution-mode",
            mode,
        ]
        if preflight_only:
            argv.append("--preflight-only")
        result = subprocess.run(argv, text=True, capture_output=True, check=False)
        if result.returncode != expect_exit:
            raise AssertionError(
                f"unexpected exit {result.returncode} != {expect_exit}\n"
                f"stdout={result.stdout}\nstderr={result.stderr}"
            )
        if expected_error is not None:
            if f"error_code={expected_error}" not in result.stderr:
                raise AssertionError(
                    f"missing error {expected_error}: {result.stderr!r}"
                )
            return None
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise AssertionError(f"non-JSON driver output: {result.stdout!r}") from error


def ceil_serialization_ps(payload: int, rate: int) -> int:
    return (payload * 8 * 1_000_000_000_000 + rate - 1) // rate


def equivalence_projection(audit: dict[str, Any]) -> dict[str, Any]:
    projected = deepcopy(audit)
    for key in (
        "execution_mode",
        "processed_event_count",
        "simulated_transit_unit_count",
        "packet_pool_allocated_count",
        "packet_pool_peak_in_use",
        "batch_pool_allocated_count",
        "batch_pool_peak_in_use",
    ):
        projected.pop(key)
    for plane in projected["planes"]:
        plane.pop("simulated_transit_unit_count")
        for port in plane["source_ports"]:
            port.pop("simulated_transit_unit_count")
    return projected


def assert_balanced(audit: dict[str, Any]) -> None:
    expected = audit["expected_payload_bytes"]
    assert audit["expected_flow_count"] == audit["completed_flow_count"]
    assert audit["sent_payload_bytes"] == expected
    assert audit["received_payload_bytes"] == expected
    assert audit["in_flight_transit_unit_count"] == 0
    assert audit["all_routes_exact"] is True
    assert audit["all_pools_returned"] is True
    assert sum(audit["per_rank_sent_payload_bytes"]) == expected
    assert sum(audit["per_rank_received_payload_bytes"]) == expected
    assert sum(audit["per_plane_payload_bytes"]) == expected


def run_dataplane_suite(harness: Harness) -> int:
    cases = 0
    for payload in (1, 1499, 1500, 1501, 3001):
        plan = single_flow_plan(f"tail-{payload}", payload, "full_packet")
        audit = harness.invoke(plan, "full_packet")
        assert audit is not None
        assert_balanced(audit)
        flow = audit["flows"][0]
        expected_packets = (payload + 1499) // 1500
        expected_tail = payload % 1500 or 1500
        sent = ceil_serialization_ps(payload, 400_000_000_000)
        assert flow["logical_packet_count"] == expected_packets
        assert flow["tail_payload_bytes"] == expected_tail
        assert flow["last_payload_sent_ps"] == sent
        assert flow["last_payload_received_ps"] == sent + 20
        cases += 1

    permutation_groups = [
        {
            "step_id": 0,
            "plane_id": 0,
            "program_epoch_id": 0,
            "configuration_id": 0,
            "src_rank": src,
            "dst_rank": dst,
            "payload_bytes": 1000 + src,
        }
        for src, dst in enumerate((1, 0, 3, 2))
    ]
    permutation = make_plan(
        "p4-permutation",
        mode="full_packet",
        node_count=4,
        plane_count=1,
        configurations=[[1, 0, 3, 2]],
        groups=permutation_groups,
        programs=[
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [0, 1, 2, 3],
                }
            ]
        ],
    )
    audit = harness.invoke(permutation, "full_packet")
    assert audit is not None
    assert_balanced(audit)
    assert audit["completed_flow_count"] == 4
    assert audit["per_rank_sent_payload_bytes"] == [1000, 1001, 1002, 1003]
    cases += 1

    simultaneous = make_plan(
        "simultaneous-two-callbacks",
        mode="full_packet",
        node_count=4,
        plane_count=1,
        configurations=[[1, 0, 3, 2]],
        groups=[
            {
                "step_id": 0,
                "plane_id": 0,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": src,
                "dst_rank": dst,
                "payload_bytes": 1500,
            }
            for src, dst in ((0, 1), (2, 3))
        ],
        programs=[
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [0, 1],
                }
            ]
        ],
    )
    audit = harness.invoke(simultaneous, "full_packet")
    assert audit is not None
    assert_balanced(audit)
    assert audit["completed_flow_count"] == 2
    assert audit["flows"][0]["last_payload_received_ps"] == audit["flows"][1][
        "last_payload_received_ps"
    ]
    cases += 1

    sparse_groups = [permutation_groups[0], permutation_groups[2]]
    sparse_groups[1] = dict(sparse_groups[1], payload_bytes=2000)
    sparse = make_plan(
        "p4-sparse",
        mode="full_packet",
        node_count=4,
        plane_count=1,
        configurations=[[1, 0, 3, 2]],
        groups=sparse_groups,
        programs=[
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [0, 1],
                }
            ]
        ],
    )
    audit = harness.invoke(sparse, "full_packet")
    assert audit is not None
    assert_balanced(audit)
    assert audit["expected_flow_count"] == 2
    assert audit["per_rank_sent_payload_bytes"] == [1000, 0, 2000, 0]
    cases += 1

    cycle_with_idle_identity = make_plan(
        "cycle-with-idle-identity",
        mode="full_packet",
        node_count=4,
        plane_count=1,
        configurations=[[0, 1, 2, 3], [1, 2, 3, 0]],
        groups=[
            {
                "step_id": 0,
                "plane_id": 0,
                "program_epoch_id": 0,
                "configuration_id": 1,
                "src_rank": 0,
                "dst_rank": 1,
                "payload_bytes": 777,
            }
        ],
        programs=[
            [
                {
                    "transition": "initial",
                    "configuration_id": 1,
                    "flow_group_ids": [0],
                }
            ]
        ],
    )
    audit = harness.invoke(cycle_with_idle_identity, "full_packet")
    assert audit is not None
    assert_balanced(audit)
    assert audit["expected_flow_count"] == 1
    cases += 1

    same_port = make_plan(
        "same-port-two-groups",
        mode="full_packet",
        node_count=2,
        plane_count=1,
        configurations=[[1, 0]],
        groups=[
            {
                "step_id": 0,
                "plane_id": 0,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": 0,
                "dst_rank": 1,
                "payload_bytes": 1000,
            },
            {
                "step_id": 0,
                "plane_id": 0,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": 0,
                "dst_rank": 1,
                "payload_bytes": 2000,
            },
        ],
        programs=[
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [0, 1],
                }
            ]
        ],
    )
    audit = harness.invoke(same_port, "full_packet")
    assert audit is not None
    assert_balanced(audit)
    assert audit["flows"][0]["last_payload_sent_ps"] < audit["flows"][1][
        "last_payload_sent_ps"
    ]
    port = audit["planes"][0]["source_ports"][0]
    assert port["max_backlog_bytes"] == 3000
    assert port["busy_intervals"] == [{"start_ps": 0, "end_ps": 60000}]
    cases += 1

    k2 = make_plan(
        "k2-same-source",
        mode="full_packet",
        node_count=2,
        plane_count=2,
        configurations=[[1, 0]],
        groups=[
            {
                "step_id": 0,
                "plane_id": 0,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": 0,
                "dst_rank": 1,
                "payload_bytes": 1500,
            },
            {
                "step_id": 0,
                "plane_id": 1,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": 0,
                "dst_rank": 1,
                "payload_bytes": 3000,
            },
        ],
        programs=[
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [0],
                }
            ],
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [1],
                }
            ],
        ],
    )
    audit = harness.invoke(k2, "full_packet")
    assert audit is not None
    assert_balanced(audit)
    assert audit["per_plane_payload_bytes"] == [1500, 3000]
    assert audit["planes"][0]["source_ports"][0]["busy_time_ps"] == 30000
    assert audit["planes"][1]["source_ports"][0]["busy_time_ps"] == 60000
    cases += 1

    limit = 1_073_741_823
    for delta in (-1, 0):
        plan = single_flow_plan(
            f"pipe-limit-{delta}", limit + delta, "full_packet", mtu=1
        )
        proof = harness.invoke(plan, "full_packet", preflight_only=True)
        assert proof is not None
        assert proof["per_pipe_transit_unit_upper_bound"][0][1] == limit + delta
        cases += 1
    over = single_flow_plan("pipe-limit-over", limit + 1, "full_packet", mtu=1)
    harness.invoke(
        over,
        "full_packet",
        preflight_only=True,
        expect_exit=2,
        expected_error="upstream_pipe_capacity_limit",
    )
    cases += 1

    huge_latency = single_flow_plan(
        "pipe-limit-huge-latency",
        limit,
        "full_packet",
        mtu=1,
        latency=10**18,
    )
    proof = harness.invoke(huge_latency, "full_packet", preflight_only=True)
    assert proof is not None
    assert proof["per_pipe_transit_unit_upper_bound"][0][1] == limit
    cases += 1

    mismatch = single_flow_plan("mode-mismatch", 1, "full_packet")
    harness.invoke(
        mismatch,
        "exact_coalesced",
        preflight_only=True,
        expect_exit=2,
        expected_error="unsupported_execution_mode",
    )
    cases += 1
    return cases


def run_equivalence_suite(harness: Harness) -> int:
    cases = 0
    for payload in (1, 1499, 1500, 1501, 3001):
        for rate in (400_000_000_000, 333_333_333_333):
            for latency in (0, 37):
                full = single_flow_plan(
                    f"equiv-{payload}-{rate}-{latency}",
                    payload,
                    "full_packet",
                    rate=rate,
                    latency=latency,
                )
                exact = deepcopy(full)
                exact["execution_mode"] = "exact_coalesced"
                BUILDER.validate_execution_plan(exact)
                full_audit = harness.invoke(full, "full_packet")
                exact_audit = harness.invoke(exact, "exact_coalesced")
                assert full_audit is not None and exact_audit is not None
                assert equivalence_projection(full_audit) == equivalence_projection(
                    exact_audit
                )
                cases += 1

    base = make_plan(
        "equiv-k2-sparse",
        mode="full_packet",
        node_count=4,
        plane_count=2,
        configurations=[[1, 0, 3, 2]],
        groups=[
            {
                "step_id": 0,
                "plane_id": 0,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": 0,
                "dst_rank": 1,
                "payload_bytes": 1501,
            },
            {
                "step_id": 0,
                "plane_id": 1,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": 0,
                "dst_rank": 1,
                "payload_bytes": 3001,
            },
            {
                "step_id": 0,
                "plane_id": 1,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": 2,
                "dst_rank": 3,
                "payload_bytes": 1499,
            },
        ],
        programs=[
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [0],
                }
            ],
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [1, 2],
                }
            ],
        ],
        rate=333_333_333_333,
        latency=37,
    )
    exact = deepcopy(base)
    exact["execution_mode"] = "exact_coalesced"
    BUILDER.validate_execution_plan(exact)
    full_audit = harness.invoke(base, "full_packet")
    exact_audit = harness.invoke(exact, "exact_coalesced")
    assert full_audit is not None and exact_audit is not None
    assert equivalence_projection(full_audit) == equivalence_projection(exact_audit)
    cases += 1

    same_port = make_plan(
        "equiv-same-port-two-groups",
        mode="full_packet",
        node_count=2,
        plane_count=1,
        configurations=[[1, 0]],
        groups=[
            {
                "step_id": 0,
                "plane_id": 0,
                "program_epoch_id": 0,
                "configuration_id": 0,
                "src_rank": 0,
                "dst_rank": 1,
                "payload_bytes": payload,
            }
            for payload in (1501, 3001)
        ],
        programs=[
            [
                {
                    "transition": "initial",
                    "configuration_id": 0,
                    "flow_group_ids": [0, 1],
                }
            ]
        ],
        rate=333_333_333_333,
        latency=37,
    )
    exact_same_port = deepcopy(same_port)
    exact_same_port["execution_mode"] = "exact_coalesced"
    BUILDER.validate_execution_plan(exact_same_port)
    full_audit = harness.invoke(same_port, "full_packet")
    exact_audit = harness.invoke(exact_same_port, "exact_coalesced")
    assert full_audit is not None and exact_audit is not None
    assert equivalence_projection(full_audit) == equivalence_projection(exact_audit)
    cases += 1

    def isolation_plan(plane0_payload: int, mode: str) -> dict[str, Any]:
        return make_plan(
            f"isolation-{plane0_payload}-{mode}",
            mode=mode,
            node_count=2,
            plane_count=2,
            configurations=[[1, 0]],
            groups=[
                {
                    "step_id": 0,
                    "plane_id": 0,
                    "program_epoch_id": 0,
                    "configuration_id": 0,
                    "src_rank": 0,
                    "dst_rank": 1,
                    "payload_bytes": plane0_payload,
                },
                {
                    "step_id": 0,
                    "plane_id": 1,
                    "program_epoch_id": 0,
                    "configuration_id": 0,
                    "src_rank": 0,
                    "dst_rank": 1,
                    "payload_bytes": 1501,
                },
            ],
            programs=[
                [
                    {
                        "transition": "initial",
                        "configuration_id": 0,
                        "flow_group_ids": [0],
                    }
                ],
                [
                    {
                        "transition": "initial",
                        "configuration_id": 0,
                        "flow_group_ids": [1],
                    }
                ],
            ],
        )

    for mode in ("full_packet", "exact_coalesced"):
        normal = harness.invoke(isolation_plan(1500, mode), mode)
        loaded = harness.invoke(isolation_plan(15000, mode), mode)
        assert normal is not None and loaded is not None
        assert normal["planes"][1] == loaded["planes"][1]
        assert normal["flows"][1] == loaded["flows"][1]
        cases += 1

    five_gib = single_flow_plan(
        "payload-5gib-exact", 5 * 1024 * 1024 * 1024, "exact_coalesced"
    )
    audit = harness.invoke(five_gib, "exact_coalesced")
    assert audit is not None
    assert_balanced(audit)
    assert audit["logical_packet_count"] == 3_579_140
    assert audit["simulated_transit_unit_count"] == 1
    assert audit["batch_pool_allocated_count"] == 1
    assert audit["packet_pool_allocated_count"] == 0
    assert audit["processed_event_count"] == 2
    cases += 1

    full_five_gib = deepcopy(five_gib)
    full_five_gib["execution_mode"] = "full_packet"
    BUILDER.validate_execution_plan(full_five_gib)
    proof = harness.invoke(full_five_gib, "full_packet", preflight_only=True)
    assert proof is not None
    assert proof["per_pipe_transit_unit_upper_bound"][0][1] == 3_579_140
    cases += 1
    return cases


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument(
        "--suite", choices=("dataplane", "equivalence"), required=True
    )
    args = parser.parse_args()
    harness = Harness(args.binary)
    try:
        if args.suite == "dataplane":
            count = run_dataplane_suite(harness)
        else:
            count = run_equivalence_suite(harness)
    finally:
        harness.close()
    print(f"phase04 {args.suite}: PASS ({count} audited cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
