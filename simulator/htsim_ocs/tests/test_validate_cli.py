"""Cross-language Phase 03 validate/capabilities contract tests."""

from __future__ import annotations

import hashlib
import json
import os
import copy
import subprocess
import tempfile
import unittest
from pathlib import Path


VALID_FIXTURES = (
    "baseline_step_lockstep",
    "exact_tail",
    "explicit_group_dag",
    "global_step_barrier",
    "one_plane_one_group",
    "one_shot_static_preinstalled",
    "payload_5gib",
    "reconfigure_new_generation",
    "retain_new_epoch_same_generation",
    "sparse_permutation_without_fake_flow",
    "swot_overlap_earliest",
    "two_groups_same_epoch_independent_release",
)

INVALID_FIXTURES = {
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


class ValidateCliTests(unittest.TestCase):
    binary: Path
    repo_root: Path

    @property
    def fixture_root(self) -> Path:
        return self.repo_root / "tests/fixtures/contracts/v2"

    def run_cli_bytes(self, *arguments: object) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [os.fsencode(self.binary), *(os.fsencode(argument) for argument in arguments)],
            check=False,
            capture_output=True,
        )

    def run_validate(self, path: Path) -> subprocess.CompletedProcess[bytes]:
        return self.run_cli_bytes("validate", "--plan", path)

    @staticmethod
    def write_document(path: Path, document: object) -> None:
        path.write_text(
            json.dumps(document, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
            + "\n",
            encoding="utf-8",
        )

    def test_capabilities_are_canonical_and_match_static_projection(self) -> None:
        result = self.run_cli_bytes("capabilities", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        document = json.loads(result.stdout.decode("utf-8"))
        canonical = (
            json.dumps(document, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
            + "\n"
        ).encode("utf-8")
        self.assertEqual(result.stdout, canonical)
        projection = {
            "schema_version": "overlap4ocs-htsim-capabilities-static-projection/v1",
            "capability_schema_id": document["schema_version"],
            "plan_schema": document["plan_schema"],
            "result_schema": document["result_schema"],
            "operation_event_schema": document["operation_event_schema"],
            "abi_limits": document["abi_limits"],
            "transport_modes": document["transport_modes"],
            "execution_modes": document["execution_modes"],
            "dependency_modes": document["dependency_modes"],
            "path_preparation_policies": document["path_preparation_policies"],
        }
        expected = (
            self.fixture_root
            / "capabilities/capabilities-v1-static-projection.json"
        ).read_bytes()
        actual = (
            json.dumps(projection, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
            + "\n"
        ).encode("utf-8")
        self.assertEqual(actual, expected)
        self.assertEqual(
            len({self.run_cli_bytes("capabilities", "--json").stdout for _ in range(10)}),
            1,
        )

    def test_all_canonical_valid_fixtures_pass(self) -> None:
        for fixture in VALID_FIXTURES:
            with self.subTest(fixture=fixture):
                path = self.fixture_root / "plans/valid" / f"{fixture}.json"
                result = self.run_validate(path)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, b"")
                self.assertIn(
                    f"sha256={hashlib.sha256(path.read_bytes()).hexdigest()}".encode(),
                    result.stdout,
                )

    def test_all_canonical_invalid_fixtures_fail_with_stable_codes(self) -> None:
        for fixture, expected_code in INVALID_FIXTURES.items():
            with self.subTest(fixture=fixture):
                path = self.fixture_root / "plans/invalid" / f"{fixture}.json"
                result = self.run_validate(path)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, b"")
                self.assertIn(f"htsim_ocs: {expected_code} at ".encode(), result.stderr)

    def test_valid_and_invalid_outputs_are_100_process_deterministic(self) -> None:
        valid = self.fixture_root / "plans/valid/global_step_barrier.json"
        invalid = self.fixture_root / "plans/invalid/event_wait_graph_cycle.json"
        for path in (valid, invalid):
            observations = {
                (result.returncode, result.stdout, result.stderr)
                for result in (self.run_validate(path) for _ in range(100))
            }
            self.assertEqual(len(observations), 1)

    def test_raw_json_rejections_and_hash_exact_bytes(self) -> None:
        source = (
            self.fixture_root / "plans/valid/one_plane_one_group.json"
        ).read_bytes()
        mutations = {
            "bom.json": (b"\xef\xbb\xbf" + source, "utf8_bom"),
            "duplicate.json": (
                b'{"case_id":"case-' + b"0" * 64 + b'",' + source[1:],
                "duplicate_object_key",
            ),
            "float.json": (source.replace(b'"seed":0', b'"seed":0.5'), "non_integer_number"),
            "negative.json": (source.replace(b'"seed":0', b'"seed":-1'), "uint64_out_of_range"),
            "nan.json": (source.replace(b'"seed":0', b'"seed":NaN'), "non_finite_number"),
            "infinity.json": (
                source.replace(b'"seed":0', b'"seed":Infinity'),
                "non_finite_number",
            ),
            "overflow.json": (
                source.replace(b'"seed":0', b'"seed":18446744073709551616'),
                "uint64_out_of_range",
            ),
            "trailing.json": (source + b"{}", "invalid_json"),
        }
        invalid_utf8 = bytearray(source)
        marker = invalid_utf8.index(b"one_plane_one_group")
        invalid_utf8[marker] = 0xFF
        mutations["invalid-utf8.json"] = (bytes(invalid_utf8), "invalid_utf8")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "空 格"
            root.mkdir()
            for name, (raw, code) in mutations.items():
                path = root / name
                path.write_bytes(raw)
                result = self.run_validate(path)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(f"htsim_ocs: {code} at ".encode(), result.stderr)

            lf_changed = root / "valid with extra LF.json"
            lf_changed.write_bytes(source + b"\n")
            result = self.run_validate(lf_changed)
            self.assertEqual(result.returncode, 0, result.stderr)
            changed_hash = hashlib.sha256(source + b"\n").hexdigest().encode()
            original_hash = hashlib.sha256(source).hexdigest().encode()
            self.assertIn(changed_hash, result.stdout)
            self.assertNotEqual(changed_hash, original_hash)

    def test_same_source_multiple_flows_remain_distinct_and_valid(self) -> None:
        source_path = (
            self.fixture_root / "plans/valid/sparse_permutation_without_fake_flow.json"
        )
        document = json.loads(source_path.read_text(encoding="utf-8"))
        second_segment = copy.deepcopy(document["logical_segments"][0])
        second_segment["segment_id"] = 1
        document["logical_segments"].append(second_segment)
        second_flow = copy.deepcopy(document["flows"][0])
        second_flow["flow_id"] = 1
        second_flow["segment_slices"][0]["segment_id"] = 1
        document["flows"].append(second_flow)
        document["flow_groups"][0]["flow_ids"] = [0, 1]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "same_source_multiple_flows_fifo.json"
            self.write_document(path, document)
            result = self.run_validate(path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b"flows=2", result.stdout)
            self.assertIn(b"payload_bytes=2000", result.stdout)

    def test_extended_structural_and_semantic_negative_matrix(self) -> None:
        simple = json.loads(
            (self.fixture_root / "plans/valid/one_plane_one_group.json").read_text()
        )
        two_plane = json.loads(
            (self.fixture_root / "plans/valid/baseline_step_lockstep.json").read_text()
        )
        reconfigure = json.loads(
            (self.fixture_root / "plans/valid/reconfigure_new_generation.json").read_text()
        )
        one_shot = json.loads(
            (self.fixture_root / "plans/valid/one_shot_static_preinstalled.json").read_text()
        )

        wrong_version = copy.deepcopy(simple)
        wrong_version["schema_version"] = "swot-execution-plan/v1"
        bad_mtu = copy.deepcopy(simple)
        bad_mtu["transport"]["mtu_bytes"] = 65536
        bad_mode = copy.deepcopy(simple)
        bad_mode["execution_mode"] = "representative"
        missing_token = copy.deepcopy(simple)
        missing_token["readiness_tokens"].pop()
        wrong_barrier = copy.deepcopy(reconfigure)
        wrong_barrier["flow_groups"][1]["depends_on_token_ids"] = [0]
        retain_mismatch = copy.deepcopy(reconfigure)
        retain_mismatch["plane_programs"][0]["epochs"][1]["transition"] = "retain"
        unsorted = copy.deepcopy(two_plane)
        unsorted["flows"][0], unsorted["flows"][1] = (
            unsorted["flows"][1],
            unsorted["flows"][0],
        )
        multi_epoch_one_shot = copy.deepcopy(one_shot)
        original_epoch = multi_epoch_one_shot["plane_programs"][0]["epochs"][0]
        original_epoch["flow_group_ids"] = [0]
        multi_epoch_one_shot["plane_programs"][0]["epochs"].append(
            {
                "configuration_id": original_epoch["configuration_id"],
                "flow_group_ids": [1],
                "path_prep_not_before_token_ids": [],
                "program_epoch_id": 1,
                "transition": "retain",
            }
        )
        multi_epoch_one_shot["flow_groups"][1]["program_epoch_id"] = 1

        result_too_large = copy.deepcopy(simple)
        result_too_large["topology"]["node_count"] = 2_000_000
        result_too_large["workload"]["rank_count"] = 2_000_000
        result_too_large["workload"]["message_bytes_per_rank"] = 0
        result_too_large["configurations"] = []
        result_too_large["logical_segments"] = []
        result_too_large["readiness_tokens"] = [
            {"producer_id": None, "token_id": 0, "token_type": "collective_start"}
        ]
        result_too_large["steps"] = []
        result_too_large["flow_groups"] = []
        result_too_large["flows"] = []
        result_too_large["plane_programs"] = [{"epochs": [], "plane_id": 0}]

        cases = {
            "wrong_version": (wrong_version, "schema_validation_error"),
            "mtu_65536": (bad_mtu, "schema_validation_error"),
            "unsupported_mode": (bad_mode, "schema_validation_error"),
            "missing_token": (missing_token, "missing_step_completion_token"),
            "wrong_global_barrier": (
                wrong_barrier,
                "global_barrier_dependency_mismatch",
            ),
            "retain_configuration_mismatch": (
                retain_mismatch,
                "retain_configuration_mismatch",
            ),
            "unsorted_global_id": (unsorted, "non_contiguous_id"),
            "one_shot_multiple_epochs": (multi_epoch_one_shot, "one_shot_not_static"),
            "result_file_size_limit": (result_too_large, "result_file_size_limit"),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, (document, expected_code) in cases.items():
                with self.subTest(name=name):
                    path = root / f"{name}.json"
                    self.write_document(path, document)
                    result = self.run_validate(path)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn(
                        f"htsim_ocs: {expected_code} at ".encode(), result.stderr
                    )

    def test_same_plane_future_epoch_and_cross_plane_cycles_are_rejected(self) -> None:
        source = json.loads(
            (self.fixture_root / "plans/valid/baseline_step_lockstep.json").read_text()
        )

        same_plane = copy.deepcopy(source)
        same_plane["strategy"] = "swot"
        same_plane["path_preparation_policy"] = "overlap_earliest"
        same_plane["dependency_mode"] = "explicit_group_dag"
        same_plane["flow_groups"][0].update(
            {"plane_id": 0, "program_epoch_id": 1, "configuration_id": 0,
             "depends_on_token_ids": [0]}
        )
        same_plane["flow_groups"][1].update(
            {"plane_id": 0, "program_epoch_id": 0, "configuration_id": 0,
             "depends_on_token_ids": [1]}
        )
        same_plane["flow_groups"][2].update(
            {"plane_id": 1, "program_epoch_id": 0, "configuration_id": 1,
             "depends_on_token_ids": [0]}
        )
        same_plane["flow_groups"][3].update(
            {"plane_id": 1, "program_epoch_id": 1, "configuration_id": 1,
             "depends_on_token_ids": [4]}
        )
        same_plane["plane_programs"] = [
            {"plane_id": 0, "epochs": [
                {"program_epoch_id": 0, "transition": "initial", "configuration_id": 0,
                 "path_prep_not_before_token_ids": [], "flow_group_ids": [1]},
                {"program_epoch_id": 1, "transition": "retain", "configuration_id": 0,
                 "path_prep_not_before_token_ids": [], "flow_group_ids": [0]},
            ]},
            {"plane_id": 1, "epochs": [
                {"program_epoch_id": 0, "transition": "initial", "configuration_id": 1,
                 "path_prep_not_before_token_ids": [], "flow_group_ids": [2]},
                {"program_epoch_id": 1, "transition": "retain", "configuration_id": 1,
                 "path_prep_not_before_token_ids": [], "flow_group_ids": [3]},
            ]},
        ]

        cross_plane = copy.deepcopy(source)
        cross_plane["strategy"] = "swot"
        cross_plane["path_preparation_policy"] = "overlap_earliest"
        cross_plane["dependency_mode"] = "explicit_group_dag"
        cross_plane["flow_groups"][0].update(
            {"plane_id": 1, "program_epoch_id": 1, "configuration_id": 0,
             "depends_on_token_ids": [0]}
        )
        cross_plane["flow_groups"][1].update(
            {"plane_id": 0, "program_epoch_id": 0, "configuration_id": 0,
             "depends_on_token_ids": [1]}
        )
        cross_plane["flow_groups"][2].update(
            {"plane_id": 1, "program_epoch_id": 0, "configuration_id": 1,
             "depends_on_token_ids": [2]}
        )
        cross_plane["flow_groups"][3].update(
            {"plane_id": 0, "program_epoch_id": 1, "configuration_id": 1,
             "depends_on_token_ids": [0]}
        )
        cross_plane["plane_programs"] = [
            {"plane_id": 0, "epochs": [
                {"program_epoch_id": 0, "transition": "initial", "configuration_id": 0,
                 "path_prep_not_before_token_ids": [], "flow_group_ids": [1]},
                {"program_epoch_id": 1, "transition": "reconfigure", "configuration_id": 1,
                 "path_prep_not_before_token_ids": [], "flow_group_ids": [3]},
            ]},
            {"plane_id": 1, "epochs": [
                {"program_epoch_id": 0, "transition": "initial", "configuration_id": 1,
                 "path_prep_not_before_token_ids": [], "flow_group_ids": [2]},
                {"program_epoch_id": 1, "transition": "reconfigure", "configuration_id": 0,
                 "path_prep_not_before_token_ids": [], "flow_group_ids": [0]},
            ]},
        ]

        with tempfile.TemporaryDirectory() as directory:
            for name, document in (("same_plane_future", same_plane),
                                   ("cross_plane_cycle", cross_plane)):
                path = Path(directory) / f"{name}.json"
                self.write_document(path, document)
                result = self.run_validate(path)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(b"htsim_ocs: event_wait_graph_cycle at ", result.stderr)

    def test_file_size_and_io_exit_classes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            oversized = root / "oversized.json"
            with oversized.open("wb") as output:
                output.truncate(67108865)
            oversized_result = self.run_validate(oversized)
            self.assertEqual(oversized_result.returncode, 2)
            self.assertIn(b"plan_file_size_limit", oversized_result.stderr)

            missing_result = self.run_validate(root / "missing.json")
            self.assertEqual(missing_result.returncode, 5)
            self.assertIn(b"input_not_regular_or_readable", missing_result.stderr)

            directory_result = self.run_validate(root)
            self.assertEqual(directory_result.returncode, 5)
            self.assertIn(b"input_not_regular_or_readable", directory_result.stderr)

    def test_phase06_exposes_run_with_io_preflight(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = self.run_cli_bytes(
                "run",
                "--plan",
                str(root / "missing-plan.json"),
                "--result",
                str(root / "result.json"),
            )
        self.assertEqual(result.returncode, 5)
        self.assertIn(b"input_not_regular_or_readable", result.stderr)


def make_suite(binary: Path, repo_root: Path) -> unittest.TestSuite:
    ValidateCliTests.binary = binary
    ValidateCliTests.repo_root = repo_root
    return unittest.defaultTestLoader.loadTestsFromTestCase(ValidateCliTests)
