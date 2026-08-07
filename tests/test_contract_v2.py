from __future__ import annotations

import unittest
from pathlib import Path

from simulator.contracts.constants import VALID_FIXTURE_IDS
from simulator.contracts.io import canonical_json_bytes, read_json_file
from simulator.contracts.validation import (
    validate_capabilities_projection,
    validate_contract_pair,
    validate_execution_plan,
    validate_operation_event,
    validate_simulation_result,
)


FIXTURE_ROOT = Path(__file__).parent / "fixtures" / "contracts" / "v2"


class ContractV2GoldenTest(unittest.TestCase):
    def test_all_canonical_plan_result_pairs(self) -> None:
        for fixture_id in VALID_FIXTURE_IDS:
            with self.subTest(fixture_id=fixture_id):
                plan_path = FIXTURE_ROOT / "plans" / "valid" / f"{fixture_id}.json"
                result_path = (
                    FIXTURE_ROOT / "results" / "success" / f"{fixture_id}.json"
                )
                pair_path = FIXTURE_ROOT / "pairs" / f"{fixture_id}.json"
                plan = read_json_file(plan_path)
                result = read_json_file(result_path)
                pair = read_json_file(pair_path)

                self.assertEqual(plan_path.read_bytes(), canonical_json_bytes(plan))
                self.assertEqual(result_path.read_bytes(), canonical_json_bytes(result))
                self.assertEqual(pair_path.read_bytes(), canonical_json_bytes(pair))
                validate_execution_plan(plan)
                validate_simulation_result(result)
                validate_contract_pair(pair, FIXTURE_ROOT)

    def test_sparse_permutation_has_only_active_traffic(self) -> None:
        plan = read_json_file(
            FIXTURE_ROOT
            / "plans"
            / "valid"
            / "sparse_permutation_without_fake_flow.json"
        )
        self.assertEqual(plan["configurations"][0]["permutation"], [1, 0, 3, 2])
        self.assertEqual(len(plan["flows"]), 1)
        self.assertEqual(
            (plan["flows"][0]["src_rank"], plan["flows"][0]["dst_rank"]),
            (0, 1),
        )

    def test_group_dependency_and_policy_examples(self) -> None:
        explicit = read_json_file(
            FIXTURE_ROOT / "plans" / "valid" / "explicit_group_dag.json"
        )
        parent_token = explicit["flow_groups"][0]["completion_token_id"]
        self.assertEqual(
            explicit["flow_groups"][1]["depends_on_token_ids"], [parent_token]
        )

        same_epoch = read_json_file(
            FIXTURE_ROOT
            / "plans"
            / "valid"
            / "two_groups_same_epoch_independent_release.json"
        )
        self.assertEqual(
            [group["depends_on_token_ids"] for group in same_epoch["flow_groups"]],
            [[0], [0], [same_epoch["flow_groups"][0]["completion_token_id"]]],
        )
        self.assertEqual(
            same_epoch["plane_programs"][0]["epochs"][0]["flow_group_ids"],
            [0, 1, 2],
        )
        pipeline_result = read_json_file(
            FIXTURE_ROOT
            / "results"
            / "success"
            / "two_groups_same_epoch_independent_release.json"
        )
        self.assertLess(
            pipeline_result["flow_groups"][2]["release_ps"],
            pipeline_result["flow_groups"][1]["completion_ps"],
        )

        expected_policies = {
            "baseline_step_lockstep": ("baseline", "step_lockstep"),
            "swot_overlap_earliest": ("swot", "overlap_earliest"),
            "one_shot_static_preinstalled": ("one_shot", "static_preinstalled"),
        }
        for fixture_id, expected in expected_policies.items():
            plan = read_json_file(
                FIXTURE_ROOT / "plans" / "valid" / f"{fixture_id}.json"
            )
            self.assertEqual(
                (plan["strategy"], plan["path_preparation_policy"]), expected
            )

    def test_transition_generation_and_packet_tail_examples(self) -> None:
        retained = read_json_file(
            FIXTURE_ROOT
            / "results"
            / "success"
            / "retain_new_epoch_same_generation.json"
        )
        reconfigured = read_json_file(
            FIXTURE_ROOT
            / "results"
            / "success"
            / "reconfigure_new_generation.json"
        )
        self.assertEqual(
            [epoch["physical_config_generation"] for epoch in retained["planes"][0]["epochs"]],
            [0, 0],
        )
        self.assertEqual(
            [epoch["physical_config_generation"] for epoch in reconfigured["planes"][0]["epochs"]],
            [0, 1],
        )

        exact_tail = read_json_file(
            FIXTURE_ROOT / "results" / "success" / "exact_tail.json"
        )
        self.assertEqual(exact_tail["flows"][0]["payload_bytes"], 1501)
        self.assertEqual(exact_tail["flows"][0]["logical_packet_count"], 2)
        self.assertEqual(exact_tail["flows"][0]["last_payload_sent_ps"], 30020)
        self.assertEqual(exact_tail["flows"][0]["last_payload_received_ps"], 30040)
        self.assertEqual(exact_tail["traffic"]["wire_bytes"], 1501)

        large = read_json_file(
            FIXTURE_ROOT / "plans" / "valid" / "payload_5gib.json"
        )
        self.assertEqual(large["flows"][0]["payload_bytes"], 5 * 1024**3)
        large_result = read_json_file(
            FIXTURE_ROOT / "results" / "success" / "payload_5gib.json"
        )
        self.assertEqual(
            large_result["timing"]["simulated_cct_ps"], 107374182420
        )

    def test_failure_results_and_capabilities_projection(self) -> None:
        expected_failures = {
            "invalid_input": "invalid_input",
            "unsupported_semantics": "unsupported_semantics",
            "deadlock": "deadlock",
            "invariant_violation": "invariant_violation",
            "stale_physical_generation": "invariant_violation",
            "max_simulation_time": "max_simulation_time",
            "max_event_count": "max_event_count",
        }
        for fixture_id, stop_reason in expected_failures.items():
            with self.subTest(fixture_id=fixture_id):
                result = read_json_file(
                    FIXTURE_ROOT / "results" / "failure" / f"{fixture_id}.json"
                )
                validate_simulation_result(result)
                self.assertEqual(result["status"], "failure")
                self.assertEqual(result["stop_reason"], stop_reason)
                self.assertIsNone(result["timing"]["simulated_cct_ps"])

        projection_path = (
            FIXTURE_ROOT
            / "capabilities"
            / "capabilities-v1-static-projection.json"
        )
        projection = read_json_file(projection_path)
        self.assertEqual(
            projection_path.read_bytes(), canonical_json_bytes(projection)
        )
        validate_capabilities_projection(projection)

    def test_operation_event_contract(self) -> None:
        event = {
            "configuration_id": 0,
            "event_index": 4,
            "event_type": "flow_complete",
            "flow_group_id": 0,
            "flow_id": 0,
            "physical_config_generation": 0,
            "plane_id": 0,
            "program_epoch_id": 0,
            "reason_code": None,
            "schema_version": "htsim-ocs-operation-event/v1",
            "step_id": 0,
            "time_ps": 1000000,
            "token_id": None,
        }
        validate_operation_event(event)

        event["token_id"] = 1
        with self.assertRaisesRegex(Exception, "flow_complete must not carry token_id"):
            validate_operation_event(event)


if __name__ == "__main__":
    unittest.main()
