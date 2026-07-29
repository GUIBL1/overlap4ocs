"""Shared file contracts between overlap4ocs and the HTSim backend."""

from simulator.contracts.io import (
    ContractDecodeError,
    SchemaValidationError,
    canonical_json_bytes,
    decode_json_bytes,
    raw_sha256,
    read_json_file,
    write_canonical_json,
)
from simulator.contracts.validation import (
    ContractValidationError,
    validate_capabilities_projection,
    validate_contract_pair,
    validate_execution_plan,
    validate_operation_event,
    validate_simulation_result,
)

__all__ = [
    "ContractDecodeError",
    "ContractValidationError",
    "SchemaValidationError",
    "canonical_json_bytes",
    "decode_json_bytes",
    "raw_sha256",
    "read_json_file",
    "validate_capabilities_projection",
    "validate_contract_pair",
    "validate_execution_plan",
    "validate_operation_event",
    "validate_simulation_result",
    "write_canonical_json",
]
