#!/usr/bin/env python3
"""Generate build-only contract identity and ABI constants."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


EXPECTED_LIMIT_KEYS = {
    "max_mtu_bytes",
    "max_payload_bytes",
    "max_pipe_inflight_transit_units",
    "max_plan_file_bytes",
    "max_result_file_bytes",
    "min_mtu_bytes",
}


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def schema_identity(path: Path, expected_id: str) -> tuple[str, str]:
    raw = path.read_bytes()
    document = json.loads(raw.decode("utf-8"))
    if document.get("$id") != expected_id:
        raise ValueError(f"schema ID mismatch for {path}")
    return expected_id, digest(raw)


def cpp_string(value: str) -> str:
    return json.dumps(value)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan-schema", type=Path, required=True)
    parser.add_argument("--result-schema", type=Path, required=True)
    parser.add_argument("--operation-schema", type=Path, required=True)
    parser.add_argument("--abi-limits", type=Path, required=True)
    parser.add_argument("--build-flags", required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    plan = schema_identity(arguments.plan_schema, "swot-execution-plan/v2")
    result = schema_identity(arguments.result_schema, "swot-simulation-result/v2")
    operation = schema_identity(
        arguments.operation_schema, "htsim-ocs-operation-event/v1"
    )
    abi_raw = arguments.abi_limits.read_bytes()
    limits = json.loads(abi_raw.decode("utf-8"))
    if set(limits) != EXPECTED_LIMIT_KEYS or any(
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < 0
        or value > (1 << 64) - 1
        for value in limits.values()
    ):
        raise ValueError("ABI limits must contain exactly six uint64 values")

    constants = {
        "kPlanSchemaId": plan[0],
        "kPlanSchemaSha256": plan[1],
        "kResultSchemaId": result[0],
        "kResultSchemaSha256": result[1],
        "kOperationEventSchemaId": operation[0],
        "kOperationEventSchemaSha256": operation[1],
        "kAbiLimitsRawSha256": digest(abi_raw),
        "kBuildFlagsSha256": digest(arguments.build_flags.encode("utf-8")),
    }
    lines = [
        "#ifndef OVERLAP4OCS_HTSIM_OCS_GENERATED_CONTRACT_H",
        "#define OVERLAP4OCS_HTSIM_OCS_GENERATED_CONTRACT_H",
        "",
        "#include <cstdint>",
        "",
        "namespace htsim_ocs::contract {",
        "",
    ]
    for name, value in constants.items():
        lines.append(f"inline constexpr char {name}[] = {cpp_string(value)};")
    for key in sorted(limits):
        name = "k" + "".join(part.capitalize() for part in key.split("_"))
        lines.append(
            f"inline constexpr std::uint64_t {name} = {limits[key]}ULL;"
        )
    lines.extend(
        [
            "",
            "}  // namespace htsim_ocs::contract",
            "",
            "#endif  // OVERLAP4OCS_HTSIM_OCS_GENERATED_CONTRACT_H",
            "",
        ]
    )
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
