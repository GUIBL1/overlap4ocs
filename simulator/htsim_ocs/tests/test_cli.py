"""CLI identity tests retained across the standalone backend phases."""

from __future__ import annotations

import hashlib
import subprocess
import unittest
from pathlib import Path


class CliContractTests(unittest.TestCase):
    binary: Path
    patchset_manifest: Path

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.binary), *arguments],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_help(self) -> None:
        result = self.run_cli("help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Usage: htsim_ocs <command>", result.stdout)
        self.assertIn("version", result.stdout)
        self.assertIn("capabilities --json", result.stdout)
        self.assertIn("validate --plan", result.stdout)

    def test_version_has_required_identity(self) -> None:
        result = self.run_cli("version")
        self.assertEqual(result.returncode, 0, result.stderr)
        patchset_sha = hashlib.sha256(
            self.patchset_manifest.read_bytes()
        ).hexdigest()
        required_lines = {
            "htsim_ocs program version": "0.5.0",
            "execution plan schema version": "swot-execution-plan/v2",
            "result schema version": "swot-simulation-result/v2",
            "csg-htsim upstream commit": (
                "841d9e7be46bb968eece766aa4b6c044c7799f67"
            ),
            "local patchset identifier": patchset_sha,
        }
        fields = dict(
            line.split(": ", 1) for line in result.stdout.splitlines()
        )
        for key, value in required_lines.items():
            self.assertEqual(fields.get(key), value)
        self.assertRegex(
            fields.get("parent repository build commit", ""),
            r"^(?:[0-9a-f]{40}|unknown)$",
        )

    def test_missing_command_fails(self) -> None:
        result = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing command", result.stderr)

    def test_unknown_command_fails(self) -> None:
        result = self.run_cli("simulate")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown command", result.stderr)

    def test_extra_argument_fails(self) -> None:
        result = self.run_cli("help", "extra")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("accepts no arguments", result.stderr)


def make_suite(binary: Path, patchset_manifest: Path) -> unittest.TestSuite:
    CliContractTests.binary = binary
    CliContractTests.patchset_manifest = patchset_manifest
    return unittest.defaultTestLoader.loadTestsFromTestCase(CliContractTests)
