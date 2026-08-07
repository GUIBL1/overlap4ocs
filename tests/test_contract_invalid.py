from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path

from simulator.contracts.io import file_sha256, read_json_file, write_canonical_json
from simulator.contracts.validation import (
    ContractValidationError,
    validate_contract_pair,
    validate_execution_plan,
    validate_simulation_result,
)


FIXTURE_ROOT = Path(__file__).parent / "fixtures" / "contracts" / "v2"


class InvalidContractTest(unittest.TestCase):
    def assert_contract_error(
        self, validator: object, path: Path, expected_code: str
    ) -> None:
        with self.assertRaises(ContractValidationError) as caught:
            validator(read_json_file(path))  # type: ignore[operator]
        self.assertEqual(caught.exception.error_code, expected_code)

    def test_invalid_plans_have_stable_error_codes(self) -> None:
        expected = {
            "baseline_path_prep_too_early": "baseline_path_prep_too_early",
            "cyclic_token": "token_not_topologically_numbered",
            "duplicate_id": "non_contiguous_id",
            "event_wait_graph_cycle": "event_wait_graph_cycle",
            "flow_route_mismatch": "flow_route_mismatch",
            "flow_slice_byte_mismatch": "flow_slice_byte_mismatch",
            "group_epoch_mismatch": "group_epoch_mismatch",
            "invalid_initial_transition": "invalid_initial_transition",
            "invalid_permutation": "invalid_permutation",
            "non_contiguous_id": "non_contiguous_id",
            "schema_unknown_field": "schema_validation_error",
            "schema_zero_payload": "schema_validation_error",
            "self_flow": "self_flow",
            "strategy_policy_mismatch": "strategy_policy_mismatch",
        }
        for fixture_id, error_code in expected.items():
            with self.subTest(fixture_id=fixture_id):
                self.assert_contract_error(
                    validate_execution_plan,
                    FIXTURE_ROOT / "plans" / "invalid" / f"{fixture_id}.json",
                    error_code,
                )

    def test_invalid_results_have_stable_error_codes(self) -> None:
        expected = {
            "failure_has_cct": "failure_has_cct",
            "result_aggregate_mismatch": "result_aggregate_mismatch",
            "schema_bad_hash": "schema_validation_error",
            "schema_unknown_field": "schema_validation_error",
            "success_missing_timing": "success_missing_timing",
        }
        for fixture_id, error_code in expected.items():
            with self.subTest(fixture_id=fixture_id):
                self.assert_contract_error(
                    validate_simulation_result,
                    FIXTURE_ROOT / "results" / "invalid" / f"{fixture_id}.json",
                    error_code,
                )

    def test_pair_rejects_result_plan_hash_mismatch(self) -> None:
        source_pair = read_json_file(
            FIXTURE_ROOT / "pairs" / "one_plane_one_group.json"
        )
        source_plan = read_json_file(
            FIXTURE_ROOT / source_pair["plan_path"]
        )
        source_result = read_json_file(
            FIXTURE_ROOT / source_pair["result_path"]
        )
        mutated_result = copy.deepcopy(source_result)
        mutated_result["plan_file_sha256"] = "0" * 64

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            plan_path = root / source_pair["plan_path"]
            result_path = root / source_pair["result_path"]
            write_canonical_json(plan_path, source_plan)
            write_canonical_json(result_path, mutated_result)
            pair = copy.deepcopy(source_pair)
            pair["plan_file_sha256"] = file_sha256(plan_path)
            pair["result_file_sha256"] = file_sha256(result_path)
            with self.assertRaises(ContractValidationError) as caught:
                validate_contract_pair(pair, root)
            self.assertEqual(
                caught.exception.error_code, "result_plan_hash_mismatch"
            )


if __name__ == "__main__":
    unittest.main()
