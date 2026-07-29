"""Frozen identifiers, enums, and paths for the v2 file ABI."""

from __future__ import annotations

from pathlib import Path


UINT64_MAX = (1 << 64) - 1
SHA256_PATTERN = r"^[0-9a-f]{64}$"
GIT_SHA_PATTERN = r"^[0-9a-f]{40}$"
CASE_ID_PATTERN = r"^case-[0-9a-f]{64}$"

PLAN_SCHEMA_ID = "swot-execution-plan/v2"
RESULT_SCHEMA_ID = "swot-simulation-result/v2"
OPERATION_EVENT_SCHEMA_ID = "htsim-ocs-operation-event/v1"
PAIR_SCHEMA_ID = "overlap4ocs-contract-pair/v1"
CAPABILITIES_PROJECTION_SCHEMA_ID = (
    "overlap4ocs-htsim-capabilities-static-projection/v1"
)

STRATEGIES = ("baseline", "one_shot", "swot")
DEPENDENCY_MODES = ("explicit_group_dag", "global_step_barrier")
PATH_PREPARATION_POLICIES = (
    "overlap_earliest",
    "static_preinstalled",
    "step_lockstep",
)
EXECUTION_MODES = ("exact_coalesced", "full_packet")
TRANSPORT_MODES = ("paper_exact",)
TRANSITIONS = ("initial", "reconfigure", "retain")

STOP_REASONS = (
    "collective_complete",
    "deadlock",
    "internal_error",
    "invalid_input",
    "invariant_violation",
    "io_error",
    "max_event_count",
    "max_simulation_time",
    "unsupported_semantics",
)

MANDATORY_INVARIANTS = (
    "all_dependency_tokens_respected",
    "all_flow_groups_complete",
    "all_flows_exact_bytes",
    "all_physical_generations_respected",
    "all_program_epochs_respected",
    "all_routes_match_configuration",
    "all_steps_complete",
    "no_cross_plane_reconfiguration_blocking",
    "no_same_plane_transfer_reconfiguration_overlap",
    "no_unexpected_drop",
    "path_preparation_policy_respected",
    "result_aggregates_match_details",
)

SIMULATOR_DIR = Path(__file__).resolve().parents[1]
SCHEMA_DIR = SIMULATOR_DIR / "schemas"
CONTRACT_DIR = SIMULATOR_DIR / "contracts"
ABI_LIMITS_PATH = CONTRACT_DIR / "abi-limits-v2.json"

SCHEMA_FILES = {
    PLAN_SCHEMA_ID: SCHEMA_DIR / "swot-execution-plan-v2.schema.json",
    RESULT_SCHEMA_ID: SCHEMA_DIR / "swot-simulation-result-v2.schema.json",
    OPERATION_EVENT_SCHEMA_ID: (
        SCHEMA_DIR / "htsim-ocs-operation-event-v1.schema.json"
    ),
    PAIR_SCHEMA_ID: CONTRACT_DIR / "contract-pair-v1.schema.json",
    CAPABILITIES_PROJECTION_SCHEMA_ID: (
        CONTRACT_DIR
        / "htsim-capabilities-static-projection-v1.schema.json"
    ),
}

VALID_FIXTURE_IDS = (
    "one_plane_one_group",
    "sparse_permutation_without_fake_flow",
    "two_groups_same_epoch_independent_release",
    "global_step_barrier",
    "explicit_group_dag",
    "baseline_step_lockstep",
    "swot_overlap_earliest",
    "one_shot_static_preinstalled",
    "retain_new_epoch_same_generation",
    "reconfigure_new_generation",
    "exact_tail",
    "payload_5gib",
)
