#!/usr/bin/env python3
"""Fresh-process byte determinism for Phase 06 result, trace, and failures."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path

from phase06_acceptance import DEFAULT_BINARY


PLAN = (
    Path(__file__).resolve().parent
    / "fixtures/v2/runtime/one_flow_exact_tail/plan-exact-coalesced.json"
)


def run_once(binary: Path, plan: Path, root: Path) -> tuple[int, bytes, bytes, bytes]:
    result = root / "result.json"
    trace = root / "operations.jsonl"
    completed = subprocess.run(
        [str(binary), "run", "--plan", str(plan), "--result", str(result),
         "--trace", str(trace)],
        check=False, capture_output=True,
    )
    return completed.returncode, completed.stdout, result.read_bytes(), trace.read_bytes()


def assert_repeated(binary: Path, plan: Path, count: int, expected_exit: int) -> None:
    reference = None
    for _ in range(count):
        with tempfile.TemporaryDirectory(prefix="phase06-determinism-") as directory:
            observed = run_once(binary, plan, Path(directory))
        assert observed[0] == expected_exit, observed
        if reference is None:
            reference = observed
        else:
            assert observed == reference


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--runs", type=int, default=100)
    args = parser.parse_args()
    binary = args.binary.resolve()
    assert_repeated(binary, PLAN, args.runs, 0)
    with tempfile.TemporaryDirectory(prefix="phase06-invalid-source-") as directory:
        invalid = Path(directory) / "invalid.json"
        invalid.write_bytes(b'{"schema_version":')
        assert_repeated(binary, invalid, args.runs, 2)
    capabilities = [
        subprocess.run(
            [str(binary), "capabilities", "--json"], check=True,
            capture_output=True,
        ).stdout
        for _ in range(args.runs)
    ]
    assert all(raw == capabilities[0] for raw in capabilities)
    print(f"phase06 determinism: PASS ({args.runs} success + {args.runs} failure + {args.runs} capabilities processes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
