#!/usr/bin/env python3
"""Honest Phase 01 test runner and vendor-integrity audit."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Iterable, Sequence

import test_cli
import test_validate_cli


REPO_ROOT = Path(__file__).resolve().parents[3]
VENDOR_ROOT = REPO_ROOT / "third_party" / "csg-htsim"
UPSTREAM_SHA = "841d9e7be46bb968eece766aa4b6c044c7799f67"
SUBTREE_IMPORT_COMMIT = "8711aff6133d618da73e44b2c2cc68c78b6dcab1"
SUBTREE_SQUASH_COMMIT = "65953695c4219be84f155e5e6846f941cc7edbd6"
PATCHSET_SCHEMA = "overlap4ocs-htsim-local-patchset/v1"


class AuditError(RuntimeError):
    """Raised when an integrity invariant is violated."""


def run_command(arguments: Sequence[str], cwd: Path = REPO_ROOT) -> str:
    result = subprocess.run(
        list(arguments),
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise AuditError(f"command failed ({' '.join(arguments)}): {detail}")
    return result.stdout


def require_file(path: Path) -> None:
    if not path.is_file():
        raise AuditError(f"required file is missing: {path}")


def find_nested_git(root: Path) -> list[Path]:
    return sorted(root.rglob(".git"))


def gitmodules_mentions_vendor(contents: str) -> bool:
    return bool(re.search(r"csg-htsim", contents, flags=re.IGNORECASE))


def parse_gitlink_entries(index_lines: Iterable[str]) -> list[str]:
    return [line for line in index_lines if line.split(maxsplit=1)[0] == "160000"]


def find_build_artifacts(root: Path) -> list[Path]:
    artifact_suffixes = {".o", ".a", ".so", ".dylib"}
    artifacts = [
        path
        for path in root.rglob("*")
        if path.is_file()
        and (path.suffix in artifact_suffixes or path.name == "libhtsim.a")
    ]
    return sorted(artifacts)


def project_link_objects(contents: str) -> set[str]:
    return {
        line.removeprefix("LOAD ").strip()
        for line in contents.splitlines()
        if line.startswith("LOAD build/") and line.rstrip().endswith(".o")
    }


def validate_link_object_allowlist(contents: str, expected: set[str]) -> None:
    actual = project_link_objects(contents)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise AuditError(f"link object allowlist mismatch: missing={missing}, extra={extra}")


def validate_subtree_message(message: str, expected_sha: str) -> None:
    required = (
        "git-subtree-dir: third_party/csg-htsim",
        f"git-subtree-split: {expected_sha}",
    )
    for trailer in required:
        if trailer not in message:
            raise AuditError(f"subtree commit is missing trailer: {trailer}")


def load_canonical_patchset(path: Path) -> tuple[dict[str, object], str]:
    require_file(path)
    raw = path.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raise AuditError("LOCAL_PATCHSET.json must not contain a UTF-8 BOM")
    try:
        manifest = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AuditError(f"invalid LOCAL_PATCHSET.json: {error}") from error

    canonical = (
        json.dumps(
            manifest,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    ).encode("utf-8")
    if raw != canonical:
        raise AuditError("LOCAL_PATCHSET.json is not canonical UTF-8 JSON + LF")
    if set(manifest) != {"patches", "schema_version", "upstream_base_sha"}:
        raise AuditError("LOCAL_PATCHSET.json has missing or unknown root fields")
    if manifest["schema_version"] != PATCHSET_SCHEMA:
        raise AuditError("LOCAL_PATCHSET.json schema version mismatch")
    if manifest["upstream_base_sha"] != UPSTREAM_SHA:
        raise AuditError("LOCAL_PATCHSET.json upstream SHA mismatch")
    patches = manifest["patches"]
    if not isinstance(patches, list):
        raise AuditError("LOCAL_PATCHSET.json patches must be an array")
    patch_ids: list[str] = []
    for patch in patches:
        if not isinstance(patch, dict) or set(patch) != {
            "affected_files",
            "patch_id",
            "semantic_effect",
            "test_ids",
            "upstream_base_sha",
        }:
            raise AuditError("local patch has missing or unknown fields")
        patch_id = patch["patch_id"]
        if not isinstance(patch_id, str) or not patch_id:
            raise AuditError("local patch ID must be a nonempty string")
        patch_ids.append(patch_id)
        if patch["upstream_base_sha"] != UPSTREAM_SHA:
            raise AuditError(f"{patch_id} upstream SHA mismatch")
        if not isinstance(patch["semantic_effect"], str) or not patch["semantic_effect"]:
            raise AuditError(f"{patch_id} semantic effect must be nonempty")
        test_ids = patch["test_ids"]
        if not isinstance(test_ids, list) or test_ids != sorted(set(test_ids)):
            raise AuditError(f"{patch_id} test IDs must be sorted and unique")
        affected_files = patch["affected_files"]
        if not isinstance(affected_files, list) or not affected_files:
            raise AuditError(f"{patch_id} must name affected files")
        paths: list[str] = []
        for affected in affected_files:
            if not isinstance(affected, dict) or set(affected) != {
                "path",
                "raw_sha256",
            }:
                raise AuditError(f"{patch_id} affected-file entry is malformed")
            relative = affected["path"]
            paths.append(relative)
            actual = hashlib.sha256((REPO_ROOT / relative).read_bytes()).hexdigest()
            if actual != affected["raw_sha256"]:
                raise AuditError(f"{patch_id} file digest mismatch: {relative}")
        if paths != sorted(set(paths)):
            raise AuditError(f"{patch_id} affected files must be sorted and unique")
    if patch_ids != sorted(set(patch_ids)):
        raise AuditError("local patches must be sorted by unique patch_id")
    return manifest, hashlib.sha256(raw).hexdigest()


def parse_version_output(binary: Path) -> dict[str, str]:
    result = subprocess.run(
        [str(binary), "version"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise AuditError(f"htsim_ocs version failed: {result.stderr.strip()}")
    try:
        return dict(line.split(": ", 1) for line in result.stdout.splitlines())
    except ValueError as error:
        raise AuditError("htsim_ocs version output is malformed") from error


def audit_vendor(binary: Path | None) -> None:
    if Path.cwd().resolve() != (REPO_ROOT / "simulator" / "htsim_ocs").resolve():
        raise AuditError("check-vendor must run through simulator/htsim_ocs/Makefile")
    if run_command(["git", "branch", "--show-current"]).strip() == "main":
        raise AuditError("vendor integration must not be developed on main")
    for relative in ("main.py", "docs", "third_party"):
        if not (REPO_ROOT / relative).exists():
            raise AuditError(f"wrong parent repository: missing {relative}")

    required_vendor_files = (
        "README.md",
        "LICENSE",
        "UPSTREAM.md",
        "LOCAL_PATCHES.md",
        "LOCAL_PATCHSET.json",
        "sim/eventlist.cpp",
        "sim/network.cpp",
        "sim/route.cpp",
        "sim/pipe.cpp",
        "sim/trigger.cpp",
    )
    for relative in required_vendor_files:
        require_file(VENDOR_ROOT / relative)

    license_text = (VENDOR_ROOT / "LICENSE").read_text(encoding="utf-8")
    if not license_text.startswith("BSD 2-Clause License"):
        raise AuditError("vendored LICENSE is not the expected BSD-2-Clause text")

    provenance = (VENDOR_ROOT / "UPSTREAM.md").read_text(encoding="utf-8")
    for expected in (
        "https://github.com/Broadcom/csg-htsim.git",
        UPSTREAM_SHA,
        "BSD-2-Clause",
        SUBTREE_IMPORT_COMMIT,
        SUBTREE_SQUASH_COMMIT,
    ):
        if expected not in provenance:
            raise AuditError(f"UPSTREAM.md is missing provenance value: {expected}")

    nested_git = find_nested_git(VENDOR_ROOT)
    if nested_git:
        raise AuditError(f"nested Git metadata found: {nested_git[0]}")

    gitmodules = REPO_ROOT / ".gitmodules"
    if gitmodules.is_file() and gitmodules_mentions_vendor(
        gitmodules.read_text(encoding="utf-8")
    ):
        raise AuditError(".gitmodules contains a csg-htsim entry")

    index_output = run_command(
        ["git", "ls-files", "-s", "third_party/csg-htsim"]
    )
    gitlinks = parse_gitlink_entries(index_output.splitlines())
    if gitlinks:
        raise AuditError(f"csg-htsim is recorded as a gitlink: {gitlinks[0]}")

    critical_tracked_files = (
        "third_party/csg-htsim/README.md",
        "third_party/csg-htsim/LICENSE",
        "third_party/csg-htsim/sim/eventlist.cpp",
        "third_party/csg-htsim/sim/network.cpp",
        "third_party/csg-htsim/sim/pipe.cpp",
    )
    tracked = set(run_command(["git", "ls-files", "third_party/csg-htsim"]).splitlines())
    missing_tracked = sorted(set(critical_tracked_files) - tracked)
    if missing_tracked:
        raise AuditError(f"vendored source is not parent-tracked: {missing_tracked[0]}")

    tracked_artifact_pattern = re.compile(
        r"(?:\.(?:o|a|so)$|/(?:htsim_ocs|htsim_[^/.]+)$)"
    )
    tracked_artifacts = sorted(path for path in tracked if tracked_artifact_pattern.search(path))
    if tracked_artifacts:
        raise AuditError(f"tracked vendor build artifact: {tracked_artifacts[0]}")
    working_artifacts = find_build_artifacts(VENDOR_ROOT)
    if working_artifacts:
        raise AuditError(f"working-tree vendor build artifact: {working_artifacts[0]}")

    run_command(["git", "cat-file", "-e", f"{SUBTREE_IMPORT_COMMIT}^{{commit}}"])
    run_command(["git", "cat-file", "-e", f"{SUBTREE_SQUASH_COMMIT}^{{commit}}"])
    ancestry = subprocess.run(
        ["git", "merge-base", "--is-ancestor", SUBTREE_SQUASH_COMMIT, "HEAD"],
        cwd=REPO_ROOT,
        check=False,
    )
    if ancestry.returncode != 0:
        raise AuditError("subtree squash commit is not an ancestor of HEAD")
    squash_message = run_command(
        ["git", "show", "-s", "--format=%B", SUBTREE_SQUASH_COMMIT]
    )
    validate_subtree_message(squash_message, UPSTREAM_SHA)

    manifest, patchset_sha = load_canonical_patchset(
        VENDOR_ROOT / "LOCAL_PATCHSET.json"
    )
    patched_paths = {
        affected["path"]
        for patch in manifest["patches"]
        for affected in patch["affected_files"]
    }
    for relative in ("README.md", "LICENSE", "sim/eventlist.cpp"):
        parent_path = f"third_party/csg-htsim/{relative}"
        if parent_path in patched_paths:
            continue
        imported_blob = run_command(
            ["git", "rev-parse", f"{SUBTREE_SQUASH_COMMIT}:{relative}"]
        ).strip()
        current_blob = run_command(["git", "hash-object", parent_path]).strip()
        if imported_blob != current_blob:
            raise AuditError(f"unrecorded vendor modification: {parent_path}")
    if binary is not None:
        fields = parse_version_output(binary)
        if fields.get("csg-htsim upstream commit") != UPSTREAM_SHA:
            raise AuditError("binary embeds the wrong upstream SHA")
        if fields.get("local patchset identifier") != patchset_sha:
            raise AuditError("binary embeds the wrong local patchset digest")


class VendorDetectorTests(unittest.TestCase):
    def test_nested_git_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".git").mkdir()
            self.assertEqual(find_nested_git(root), [root / ".git"])

    def test_gitmodule_entry_is_detected(self) -> None:
        contents = '[submodule "csg-htsim"]\n\tpath = third_party/csg-htsim\n'
        self.assertTrue(gitmodules_mentions_vendor(contents))

    def test_gitlink_is_detected(self) -> None:
        lines = ["160000 deadbeef 0\tthird_party/csg-htsim"]
        self.assertEqual(parse_gitlink_entries(lines), lines)

    def test_build_artifact_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "sim" / "eventlist.o"
            artifact.parent.mkdir()
            artifact.touch()
            self.assertEqual(find_build_artifacts(Path(directory)), [artifact])

    def test_missing_license_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(AuditError):
                require_file(Path(directory) / "LICENSE")

    def test_bad_subtree_sha_is_detected(self) -> None:
        message = "git-subtree-dir: third_party/csg-htsim\n"
        with self.assertRaises(AuditError):
            validate_subtree_message(message, UPSTREAM_SHA)

    def test_forbidden_logger_object_is_detected(self) -> None:
        with self.assertRaises(AuditError):
            validate_link_object_allowlist(
                "LOAD build/core/eventlist.o\nLOAD build/core/loggers.o\n",
                {"build/core/eventlist.o"},
            )

    def test_forbidden_protocol_object_is_detected(self) -> None:
        with self.assertRaises(AuditError):
            validate_link_object_allowlist(
                "LOAD build/core/eventlist.o\nLOAD build/core/tcp.o\n",
                {"build/core/eventlist.o"},
            )


class BuildIntegrationTests(unittest.TestCase):
    binary: Path
    probe_binary: Path
    link_maps: tuple[Path, Path]

    def test_core_probe_reaches_and_frees_once(self) -> None:
        result = subprocess.run(
            [str(self.probe_binary)],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("received=1 freed=1", result.stdout)

    def test_link_maps_contain_only_minimal_closure(self) -> None:
        required_objects = (
            "htsim_logged_shim.o",
            "eventlist.o",
            "network.o",
            "route.o",
            "pipe.o",
            "trigger.o",
        )
        forbidden_objects = (
            "config.o",
            "queue.o",
            "switch.o",
            "loggers.o",
            "logfile.o",
            "tcp.o",
            "ndp.o",
            "roce.o",
            "eqds.o",
        )
        for link_map in self.link_maps:
            contents = link_map.read_text(encoding="utf-8", errors="replace")
            for object_name in required_objects:
                self.assertIn(object_name, contents, link_map)
            for object_name in forbidden_objects:
                self.assertNotRegex(
                    contents,
                    rf"(?:^|[/\s]){re.escape(object_name)}(?:\s|$)",
                    link_map,
                )
            common = {
                "build/htsim_logged_shim.o",
                "build/core/eventlist.o",
                "build/core/network.o",
                "build/core/route.o",
                "build/core/pipe.o",
                "build/core/trigger.o",
            }
            expected = (
                common
                | {
                    "build/sha256.o",
                    "build/ocs_execution_plan.o",
                    "build/ocs_plan_parser.o",
                    "build/ocs_packet_flow_bridge.o",
                    "build/ocs_packet.o",
                    "build/ocs_flow.o",
                    "build/ocs_execution_mode.o",
                    "build/ocs_switch.o",
                    "build/ocs_sink.o",
                    "build/ocs_route_table.o",
                    "build/ocs_port_serializer.o",
                    "build/ocs_plane_dataplane.o",
                    "build/ocs_topology.o",
                    "build/ocs_trace_collector.o",
                    "build/ocs_flow_group.o",
                    "build/ocs_dependency_tracker.o",
                    "build/ocs_program_epoch.o",
                    "build/ocs_reconfiguration.o",
                    "build/ocs_plane_runtime.o",
                    "build/ocs_guards.o",
                    "build/ocs_watchdog.o",
                    "build/ocs_coordinator.o",
                    "build/ocs_result_writer.o",
                    "build/main_ocs.o",
                }
                if link_map.name == "htsim_ocs.map"
                else common | {"build/test_link_closure.o"}
            )
            validate_link_object_allowlist(contents, expected)

    def test_undefined_symbols_have_no_forbidden_domains(self) -> None:
        output = run_command(["nm", "-C", "-u", str(self.binary)])
        for domain in ("BaseQueue", "Tcp", "Ndp", "Roce", "Logfile"):
            self.assertNotIn(domain, output)

    def test_intentional_failure_propagates_nonzero(self) -> None:
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), "--intentional-failure"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.assertNotEqual(result.returncode, 0)


class IntentionalFailure(unittest.TestCase):
    def test_failure_fixture(self) -> None:
        self.fail("intentional runner failure fixture")


def run_suite(suite: unittest.TestSuite) -> int:
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


def resolved_path(value: str | None) -> Path | None:
    return None if value is None else Path(value).resolve()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary")
    parser.add_argument("--probe-binary")
    parser.add_argument("--link-map")
    parser.add_argument("--probe-link-map")
    parser.add_argument("--check-vendor", action="store_true")
    parser.add_argument("--intentional-failure", action="store_true")
    arguments = parser.parse_args()

    if arguments.intentional_failure:
        return run_suite(
            unittest.defaultTestLoader.loadTestsFromTestCase(IntentionalFailure)
        )

    binary = resolved_path(arguments.binary)
    if arguments.check_vendor:
        try:
            audit_vendor(binary)
        except AuditError as error:
            print(f"check-vendor: FAIL: {error}", file=sys.stderr)
            return 1
        print("check-vendor: PASS")
        return 0

    required_paths = {
        "--binary": binary,
        "--probe-binary": resolved_path(arguments.probe_binary),
        "--link-map": resolved_path(arguments.link_map),
        "--probe-link-map": resolved_path(arguments.probe_link_map),
    }
    missing_arguments = [name for name, path in required_paths.items() if path is None]
    if missing_arguments:
        parser.error(f"required for normal tests: {', '.join(missing_arguments)}")
    for name, path in required_paths.items():
        if not path or not path.is_file():
            parser.error(f"{name} does not name a file: {path}")

    probe_binary = required_paths["--probe-binary"]
    link_map = required_paths["--link-map"]
    probe_link_map = required_paths["--probe-link-map"]
    assert binary is not None
    assert probe_binary is not None
    assert link_map is not None
    assert probe_link_map is not None

    BuildIntegrationTests.binary = binary
    BuildIntegrationTests.probe_binary = probe_binary
    BuildIntegrationTests.link_maps = (link_map, probe_link_map)

    suite = unittest.TestSuite()
    suite.addTests(
        test_cli.make_suite(binary, VENDOR_ROOT / "LOCAL_PATCHSET.json")
    )
    suite.addTests(test_validate_cli.make_suite(binary, REPO_ROOT))
    suite.addTests(
        unittest.defaultTestLoader.loadTestsFromTestCase(VendorDetectorTests)
    )
    suite.addTests(
        unittest.defaultTestLoader.loadTestsFromTestCase(BuildIntegrationTests)
    )
    return run_suite(suite)


if __name__ == "__main__":
    sys.exit(main())
