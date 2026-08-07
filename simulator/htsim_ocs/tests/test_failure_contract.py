#!/usr/bin/env python3
"""Phase 06 failure-result, atomic commit, and interruption acceptance."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import signal
import subprocess
import tempfile
import time
from pathlib import Path

from phase06_acceptance import DEFAULT_BINARY, REPO_ROOT, canonical

from simulator.contracts.validation import validate_simulation_result


VALID_PLAN = REPO_ROOT / "tests/fixtures/contracts/v2/plans/valid/one_plane_one_group.json"
INVALID_PLAN = REPO_ROOT / "tests/fixtures/contracts/v2/plans/invalid/invalid_permutation.json"
FIG5_FULL = (
    Path(__file__).resolve().parent
    / "fixtures/v2/runtime/paper_fig5_strawman/plan-full-packet.json"
)


def formal_run(binary: Path, plan: Path, root: Path, *, trace: bool = True) -> tuple[subprocess.CompletedProcess[bytes], dict, bytes]:
    result = root / "result.json"
    trace_path = root / "operations.jsonl"
    command = [str(binary), "run", "--plan", str(plan), "--result", str(result)]
    if trace:
        command.extend(["--trace", str(trace_path)])
    completed = subprocess.run(command, check=False, capture_output=True)
    assert result.is_file(), completed.stderr
    document = json.loads(result.read_text())
    validate_simulation_result(document)
    return completed, document, trace_path.read_bytes() if trace else b""


def assert_untrusted_failures(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="phase06-invalid-") as directory:
        root = Path(directory)
        broken = root / "broken.json"
        broken.write_bytes(b'{"schema_version":')
        completed, result, trace = formal_run(binary, broken, root)
        assert completed.returncode == 2
        assert result["status"] == "failure" and result["stop_reason"] == "invalid_input"
        assert result["plan_file_sha256"] == hashlib.sha256(broken.read_bytes()).hexdigest()
        assert result["timing"]["simulated_cct_ps"] is None
        assert result["provenance"] is None
        assert trace == b""
    with tempfile.TemporaryDirectory(prefix="phase06-schema-") as directory:
        completed, result, _ = formal_run(binary, INVALID_PLAN, Path(directory))
        assert completed.returncode == 2
        assert result["error"]["error_code"] == "schema_validation_error"
        assert result["timing"]["simulated_cct_ps"] is None
    with tempfile.TemporaryDirectory(prefix="phase06-plan-limit-") as directory:
        root = Path(directory)
        oversized = root / "oversized.json"
        with oversized.open("wb") as output:
            output.truncate(67_108_865)
        completed, result, _ = formal_run(binary, oversized, root)
        assert completed.returncode == 2
        assert result["error"]["error_code"] == "plan_file_size_limit"
        assert result["plan_file_sha256"] is None


def assert_result_size_preflight(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="phase06-result-limit-") as directory:
        root = Path(directory)
        plan = json.loads(VALID_PLAN.read_text())
        base_flow = plan["flows"][0]
        base_segment = plan["logical_segments"][0]
        flow_count = 22_000
        flows = []
        segments = []
        for flow_id in range(flow_count):
            flow = copy.deepcopy(base_flow)
            flow["flow_id"] = flow_id
            flow["segment_slices"][0]["segment_id"] = flow_id
            flows.append(flow)
            segment = copy.deepcopy(base_segment)
            segment["segment_id"] = flow_id
            segments.append(segment)
        plan["flows"] = flows
        plan["logical_segments"] = segments
        plan["flow_groups"][0]["flow_ids"] = list(range(flow_count))
        path = root / "result-too-large.json"
        path.write_bytes(canonical(plan))
        completed, result, _ = formal_run(binary, path, root)
        assert completed.returncode == 2
        assert result["stop_reason"] == "unsupported_semantics"
        assert result["error"]["error_code"] == "result_file_size_limit"
        assert result["timing"]["simulated_cct_ps"] is None


def assert_unsupported(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="phase06-unsupported-") as directory:
        root = Path(directory)
        plan = json.loads(VALID_PLAN.read_text())
        # One more full packet than the frozen signed-32-bit-safe Pipe bound.
        payload = (1_073_741_823 + 1) * plan["transport"]["mtu_bytes"]
        plan["execution_mode"] = "full_packet"
        plan["flows"][0]["payload_bytes"] = payload
        plan["flows"][0]["segment_slices"][0]["length_bytes"] = payload
        plan["logical_segments"][0]["length_bytes"] = payload
        plan["workload"]["message_bytes_per_rank"] = payload
        path = root / "unsupported.json"
        path.write_bytes(canonical(plan))
        completed, result, _ = formal_run(binary, path, root)
        assert completed.returncode == 2
        assert result["stop_reason"] == "unsupported_semantics"
        assert result["error"]["error_code"] == "upstream_pipe_capacity_limit"
        assert result["provenance"] is not None
        assert result["traffic"] is None


def assert_runtime_fault_results(driver: Path) -> None:
    for fault, reason, code in (
        ("suppress-flow-completion=0", "deadlock", "no_progress"),
        ("duplicate-flow-completion=0", "invariant_violation", "dependency_violation"),
    ):
        completed = subprocess.run(
            [str(driver), "--plan", str(VALID_PLAN), "--fault", fault],
            check=False, capture_output=True,
        )
        assert completed.returncode == 3, completed.stderr
        result = json.loads(completed.stdout)
        validate_simulation_result(result)
        assert result["stop_reason"] == reason
        assert result["error"]["error_code"] == code
        assert result["timing"]["simulated_cct_ps"] is None
        assert result["blocked_state"] is not None
        if reason == "invariant_violation":
            assert result["invariants"]["all_dependency_tokens_respected"] is False


def popen_run(binary: Path, result: Path, trace: Path | None = None) -> subprocess.Popen[bytes]:
    command = [str(binary), "run", "--plan", str(FIG5_FULL), "--result", str(result)]
    if trace is not None:
        command.extend(["--trace", str(trace)])
    return subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def wait_until_runtime(process: subprocess.Popen[bytes]) -> None:
    time.sleep(0.02)
    assert process.poll() is None, "race fixture completed before fault injection"


def assert_no_overwrite_and_preflight(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="phase06-existing-") as directory:
        root = Path(directory)
        result = root / "result.json"
        sentinel = b"existing-result\n"
        result.write_bytes(sentinel)
        completed = subprocess.run(
            [str(binary), "run", "--plan", str(VALID_PLAN), "--result", str(result)],
            check=False, capture_output=True,
        )
        assert completed.returncode == 2
        assert result.read_bytes() == sentinel
    with tempfile.TemporaryDirectory(prefix="phase06-unwritable-") as directory:
        root = Path(directory)
        output = root / "locked"
        output.mkdir()
        output.chmod(0o555)
        try:
            completed = subprocess.run(
                [str(binary), "run", "--plan", str(VALID_PLAN),
                 "--result", str(output / "result.json")],
                check=False, capture_output=True,
            )
            assert completed.returncode == 5
            assert not (output / "result.json").exists()
        finally:
            output.chmod(0o755)


def assert_trace_commit_failures(binary: Path) -> None:
    # Temp creation/write failure after successful preflight.
    with tempfile.TemporaryDirectory(prefix="phase06-trace-write-") as directory:
        root = Path(directory)
        trace_dir = root / "trace"
        trace_dir.mkdir()
        result = root / "result.json"
        trace = trace_dir / "operations.jsonl"
        process = popen_run(binary, result, trace)
        wait_until_runtime(process)
        trace_dir.chmod(0o555)
        try:
            _, stderr = process.communicate(timeout=60)
        finally:
            trace_dir.chmod(0o755)
        assert process.returncode == 5, stderr
        failure = json.loads(result.read_text())
        validate_simulation_result(failure)
        assert failure["stop_reason"] == "io_error"
        assert failure["error"]["error_code"] == "output_commit_failed"
        assert not trace.exists()

    # No-replace rename failure must not publish the computed success result.
    with tempfile.TemporaryDirectory(prefix="phase06-trace-rename-") as directory:
        root = Path(directory)
        result = root / "result.json"
        trace = root / "operations.jsonl"
        process = popen_run(binary, result, trace)
        wait_until_runtime(process)
        sentinel = b"external-trace\n"
        trace.write_bytes(sentinel)
        _, stderr = process.communicate(timeout=60)
        assert process.returncode == 5, stderr
        assert trace.read_bytes() == sentinel
        failure = json.loads(result.read_text())
        validate_simulation_result(failure)
        assert failure["stop_reason"] == "io_error"


def assert_result_commit_failures(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="phase06-result-write-") as directory:
        root = Path(directory)
        output = root / "output"
        output.mkdir()
        result = output / "result.json"
        process = popen_run(binary, result)
        wait_until_runtime(process)
        output.chmod(0o555)
        try:
            _, stderr = process.communicate(timeout=60)
        finally:
            output.chmod(0o755)
        assert process.returncode == 5, stderr
        assert not result.exists()

    with tempfile.TemporaryDirectory(prefix="phase06-result-rename-") as directory:
        root = Path(directory)
        result = root / "result.json"
        process = popen_run(binary, result)
        wait_until_runtime(process)
        sentinel = b"external-result\n"
        result.write_bytes(sentinel)
        _, stderr = process.communicate(timeout=60)
        assert process.returncode == 5, stderr
        assert result.read_bytes() == sentinel


def assert_interruption(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="phase06-sigint-") as directory:
        root = Path(directory)
        result = root / "result.json"
        trace = root / "operations.jsonl"
        process = popen_run(binary, result, trace)
        wait_until_runtime(process)
        process.send_signal(signal.SIGINT)
        process.communicate(timeout=10)
        assert process.returncode == -signal.SIGINT
        assert not result.exists()
        assert not trace.exists()
        assert not list(root.glob(".*.tmp.*"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument(
        "--result-writer-driver", type=Path,
        default=Path(__file__).resolve().parent.parent / "build/test_result_writer",
    )
    args = parser.parse_args()
    binary = args.binary.resolve()
    driver = args.result_writer_driver.resolve()
    assert_untrusted_failures(binary)
    assert_result_size_preflight(binary)
    assert_unsupported(binary)
    assert_runtime_fault_results(driver)
    assert_no_overwrite_and_preflight(binary)
    assert_trace_commit_failures(binary)
    assert_result_commit_failures(binary)
    assert_interruption(binary)
    print("phase06 failure/atomicity contract: PASS (14 injected paths)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
