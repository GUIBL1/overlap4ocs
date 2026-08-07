from __future__ import annotations

import copy
import hashlib
import tempfile
import unittest
from pathlib import Path

from simulator.contracts.constants import ABI_LIMITS_PATH, UINT64_MAX
from simulator.contracts.io import (
    ContractDecodeError,
    canonical_json_bytes,
    decode_json_bytes,
    file_sha256,
    raw_sha256,
    read_raw_file,
    read_json_file,
    write_canonical_json,
)
from simulator.contracts.validation import (
    ContractValidationError,
    validate_execution_plan,
)


FIXTURE_ROOT = Path(__file__).parent / "fixtures" / "contracts" / "v2"
REPO_ROOT = Path(__file__).resolve().parents[1]


class ContractBytesAndHashTest(unittest.TestCase):
    def assert_decode_error(self, raw: bytes, expected_code: str) -> None:
        with self.assertRaises(ContractDecodeError) as caught:
            decode_json_bytes(raw)
        self.assertEqual(caught.exception.error_code, expected_code)

    def test_strict_decoder_rejects_ambiguous_or_noncanonical_numbers(self) -> None:
        cases = (
            (b'{"a":1,"a":2}\n', "duplicate_object_key"),
            (b"\xef\xbb\xbf{}\n", "utf8_bom"),
            (b'{"a":"\xff"}\n', "invalid_utf8"),
            (b'{"a":1.0}\n', "non_integer_number"),
            (b'{"a":NaN}\n', "non_finite_number"),
            (b'{"a":-1}\n', "uint64_out_of_range"),
            (f'{{"a":{UINT64_MAX + 1}}}\n'.encode(), "uint64_out_of_range"),
            (b'{"a":1} trailing\n', "invalid_json"),
        )
        for raw, expected_code in cases:
            with self.subTest(expected_code=expected_code):
                self.assert_decode_error(raw, expected_code)

    def test_raw_hash_includes_whitespace_and_terminal_lf(self) -> None:
        compact = b'{"a":1}\n'
        spaced = b'{"a": 1}\n'
        no_lf = b'{"a":1}'
        self.assertNotEqual(raw_sha256(compact), raw_sha256(spaced))
        self.assertNotEqual(raw_sha256(compact), raw_sha256(no_lf))
        self.assertEqual(canonical_json_bytes({"a": 1}), compact)

    def test_bounded_reader_rejects_before_returning_oversized_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "plan.json"
            path.write_bytes(b"{}\n")
            with self.assertRaises(ContractDecodeError) as caught:
                read_raw_file(path, max_bytes=2)
            self.assertEqual(caught.exception.error_code, "file_size_limit")

    def test_unicode_and_space_path_atomic_round_trip(self) -> None:
        document = {"label": "路径 Δ", "maximum": UINT64_MAX}
        with tempfile.TemporaryDirectory(prefix="ocs contract ") as directory:
            path = Path(directory) / "含 空格" / "计划.json"
            digest = write_canonical_json(path, document)
            self.assertEqual(read_json_file(path), document)
            self.assertEqual(digest, file_sha256(path))
            self.assertEqual(path.read_bytes(), canonical_json_bytes(document))

    def test_abi_limits_are_single_source_and_uint64_safe(self) -> None:
        limits = read_json_file(ABI_LIMITS_PATH)
        self.assertEqual(
            set(limits),
            {
                "max_plan_file_bytes",
                "max_result_file_bytes",
                "min_mtu_bytes",
                "max_mtu_bytes",
                "max_payload_bytes",
                "max_pipe_inflight_transit_units",
            },
        )
        self.assertEqual(limits["min_mtu_bytes"], 1)
        self.assertEqual(limits["max_mtu_bytes"], 65535)
        self.assertEqual(limits["max_payload_bytes"], UINT64_MAX)
        self.assertEqual(limits["max_pipe_inflight_transit_units"], 1073741823)
        self.assertEqual(ABI_LIMITS_PATH.read_bytes(), canonical_json_bytes(limits))

    def test_uint64_max_payload_and_checked_aggregate_overflow(self) -> None:
        base = read_json_file(
            FIXTURE_ROOT / "plans" / "valid" / "one_plane_one_group.json"
        )
        maximum = copy.deepcopy(base)
        maximum["workload"]["message_bytes_per_rank"] = UINT64_MAX
        maximum["logical_segments"][0]["length_bytes"] = UINT64_MAX
        maximum["flows"][0]["payload_bytes"] = UINT64_MAX
        maximum["flows"][0]["segment_slices"][0]["length_bytes"] = UINT64_MAX
        validate_execution_plan(maximum)

        overflowing = read_json_file(
            FIXTURE_ROOT
            / "plans"
            / "valid"
            / "two_groups_same_epoch_independent_release.json"
        )
        for index in (0, 1):
            overflowing["logical_segments"][index]["length_bytes"] = UINT64_MAX
            overflowing["flows"][index]["payload_bytes"] = UINT64_MAX
            overflowing["flows"][index]["segment_slices"][0][
                "length_bytes"
            ] = UINT64_MAX
        with self.assertRaises(ContractValidationError) as caught:
            validate_execution_plan(overflowing)
        self.assertEqual(caught.exception.error_code, "uint64_overflow")

    def test_sha256sum_manifest_and_no_self_hash(self) -> None:
        expected = {}
        for line in (FIXTURE_ROOT / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
            digest, relative = line.split("  ", 1)
            expected[relative] = digest
        json_paths = sorted(FIXTURE_ROOT.rglob("*.json"))
        self.assertEqual(set(expected), {p.relative_to(FIXTURE_ROOT).as_posix() for p in json_paths})
        for path in json_paths:
            relative = path.relative_to(FIXTURE_ROOT).as_posix()
            self.assertEqual(file_sha256(path), expected[relative])
        for path in (FIXTURE_ROOT / "plans" / "valid").glob("*.json"):
            self.assertNotIn("plan_file_sha256", read_json_file(path))
        for path in (FIXTURE_ROOT / "results").rglob("*.json"):
            self.assertNotIn("result_file_sha256", read_json_file(path))

    def test_nist_sha256_vectors(self) -> None:
        vectors = {
            b"": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            b"abc": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq": (
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
            ),
        }
        for message, expected in vectors.items():
            self.assertEqual(hashlib.sha256(message).hexdigest(), expected)

    def test_vendored_json_parser_checksums(self) -> None:
        vendor = REPO_ROOT / "simulator" / "htsim_ocs" / "third_party" / "nlohmann"
        self.assertEqual(
            file_sha256(vendor / "json.hpp"),
            "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63",
        )
        self.assertEqual(
            file_sha256(vendor / "LICENSE.MIT"),
            "46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603",
        )


if __name__ == "__main__":
    unittest.main()
