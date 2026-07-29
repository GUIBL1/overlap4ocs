"""Reference semantic validators for the frozen cross-language contracts."""

from __future__ import annotations

from collections import defaultdict, deque
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

from simulator.contracts.constants import (
    ABI_LIMITS_PATH,
    CAPABILITIES_PROJECTION_SCHEMA_ID,
    DEPENDENCY_MODES,
    EXECUTION_MODES,
    MANDATORY_INVARIANTS,
    OPERATION_EVENT_SCHEMA_ID,
    PAIR_SCHEMA_ID,
    PATH_PREPARATION_POLICIES,
    PLAN_SCHEMA_ID,
    RESULT_SCHEMA_ID,
    SCHEMA_FILES,
    STRATEGIES,
    TRANSPORT_MODES,
    UINT64_MAX,
)
from simulator.contracts.io import (
    SchemaValidationError,
    file_sha256,
    read_json_file,
    validate_against_schema,
)


class ContractValidationError(ValueError):
    """Stable semantic failure with a machine-readable code and pointer."""

    def __init__(self, error_code: str, json_pointer: str, message: str):
        super().__init__(message)
        self.error_code = error_code
        self.json_pointer = json_pointer


def _fail(error_code: str, pointer: str, message: str) -> None:
    raise ContractValidationError(error_code, pointer, message)


def _schema(document: Any, schema_id: str) -> None:
    try:
        validate_against_schema(document, schema_id)
    except SchemaValidationError as error:
        raise ContractValidationError(
            error.error_code, error.json_pointer, str(error)
        ) from error


def _require_contiguous_ids(
    items: list[dict[str, Any]], key: str, pointer: str
) -> None:
    actual = [item[key] for item in items]
    expected = list(range(len(items)))
    if actual != expected:
        _fail(
            "non_contiguous_id",
            pointer,
            f"{key} values must be sorted and contiguous from zero",
        )


def _require_sorted_unique(values: list[int], pointer: str) -> None:
    if values != sorted(set(values)):
        _fail("noncanonical_id_array", pointer, "ID array must be sorted and unique")


def _checked_sum(values: Iterable[int], pointer: str) -> int:
    total = 0
    for value in values:
        if value > UINT64_MAX - total:
            _fail("uint64_overflow", pointer, "checked uint64 sum overflow")
        total += value
    return total


def _require_index(value: int, size: int, pointer: str, entity: str) -> None:
    if value >= size:
        _fail("reference_out_of_range", pointer, f"unknown {entity}: {value}")


def _assert_acyclic(edges: dict[str, set[str]], pointer: str) -> None:
    nodes: set[str] = set(edges)
    for destinations in edges.values():
        nodes.update(destinations)
    indegree = {node: 0 for node in nodes}
    for destinations in edges.values():
        for destination in destinations:
            indegree[destination] += 1
    ready = deque(sorted(node for node, degree in indegree.items() if degree == 0))
    seen = 0
    while ready:
        node = ready.popleft()
        seen += 1
        for destination in sorted(edges.get(node, ())):
            indegree[destination] -= 1
            if indegree[destination] == 0:
                ready.append(destination)
    if seen != len(nodes):
        _fail("event_wait_graph_cycle", pointer, "combined event wait-for graph has a cycle")


def _policy_for_strategy(strategy: str) -> str:
    return {
        "baseline": "step_lockstep",
        "one_shot": "static_preinstalled",
        "swot": "overlap_earliest",
    }[strategy]


def validate_execution_plan(plan: dict[str, Any]) -> None:
    """Validate schema and all algorithm-independent Plan v2 semantics."""

    _schema(plan, PLAN_SCHEMA_ID)
    if plan["path_preparation_policy"] != _policy_for_strategy(plan["strategy"]):
        _fail(
            "strategy_policy_mismatch",
            "/path_preparation_policy",
            "strategy and path preparation policy do not match",
        )

    topology = plan["topology"]
    node_count = topology["node_count"]
    plane_count = topology["plane_count"]
    if plan["workload"]["rank_count"] != node_count:
        _fail(
            "rank_count_mismatch",
            "/workload/rank_count",
            "workload rank_count must equal topology node_count",
        )

    configurations = plan["configurations"]
    segments = plan["logical_segments"]
    tokens = plan["readiness_tokens"]
    steps = plan["steps"]
    groups = plan["flow_groups"]
    flows = plan["flows"]
    programs = plan["plane_programs"]

    for collection, key, pointer in (
        (configurations, "configuration_id", "/configurations"),
        (segments, "segment_id", "/logical_segments"),
        (tokens, "token_id", "/readiness_tokens"),
        (steps, "step_id", "/steps"),
        (groups, "flow_group_id", "/flow_groups"),
        (flows, "flow_id", "/flows"),
        (programs, "plane_id", "/plane_programs"),
    ):
        _require_contiguous_ids(collection, key, pointer)

    if len(programs) != plane_count:
        _fail(
            "plane_program_count_mismatch",
            "/plane_programs",
            "one plane program is required for every plane",
        )

    for configuration in configurations:
        configuration_id = configuration["configuration_id"]
        permutation = configuration["permutation"]
        if len(permutation) != node_count or sorted(permutation) != list(
            range(node_count)
        ):
            _fail(
                "invalid_permutation",
                f"/configurations/{configuration_id}/permutation",
                "configuration permutation must be a complete rank bijection",
            )

    for segment in segments:
        segment_id = segment["segment_id"]
        for field in ("origin_rank_ids", "final_destination_rank_ids"):
            values = segment[field]
            _require_sorted_unique(values, f"/logical_segments/{segment_id}/{field}")
            if any(rank >= node_count for rank in values):
                _fail(
                    "rank_out_of_range",
                    f"/logical_segments/{segment_id}/{field}",
                    "segment rank is outside topology",
                )
        if segment["buffer_offset_bytes"] > UINT64_MAX - segment["length_bytes"]:
            _fail(
                "segment_range_overflow",
                f"/logical_segments/{segment_id}",
                "segment byte range overflows uint64",
            )

    if tokens[0] != {
        "token_id": 0,
        "token_type": "collective_start",
        "producer_id": None,
    }:
        _fail(
            "invalid_collective_start_token",
            "/readiness_tokens/0",
            "token 0 must be the unique collective_start token",
        )
    if sum(token["token_type"] == "collective_start" for token in tokens) != 1:
        _fail(
            "duplicate_collective_start_token",
            "/readiness_tokens",
            "exactly one collective_start token is required",
        )

    group_token_by_group: dict[int, int] = {}
    step_token_by_step: dict[int, int] = {}
    for token in tokens:
        token_id = token["token_id"]
        token_type = token["token_type"]
        producer_id = token["producer_id"]
        if token_type == "collective_start":
            if producer_id is not None:
                _fail("token_producer_mismatch", f"/readiness_tokens/{token_id}", "collective_start has no producer")
        elif token_type == "flow_group_complete":
            if producer_id is None or producer_id >= len(groups):
                _fail("token_producer_mismatch", f"/readiness_tokens/{token_id}/producer_id", "invalid group token producer")
            if producer_id in group_token_by_group:
                _fail("duplicate_token_producer", f"/readiness_tokens/{token_id}", "group has more than one completion token")
            group_token_by_group[producer_id] = token_id
        else:
            if producer_id is None or producer_id >= len(steps):
                _fail("token_producer_mismatch", f"/readiness_tokens/{token_id}/producer_id", "invalid step token producer")
            if producer_id in step_token_by_step:
                _fail("duplicate_token_producer", f"/readiness_tokens/{token_id}", "step has more than one completion token")
            step_token_by_step[producer_id] = token_id
    if set(group_token_by_group) != set(range(len(groups))):
        _fail("missing_group_completion_token", "/readiness_tokens", "every group needs exactly one completion token")
    if set(step_token_by_step) != set(range(len(steps))):
        _fail("missing_step_completion_token", "/readiness_tokens", "every step needs exactly one completion token")

    group_step_membership: dict[int, int] = {}
    for step in steps:
        step_id = step["step_id"]
        _require_sorted_unique(step["flow_group_ids"], f"/steps/{step_id}/flow_group_ids")
        if step["completion_token_id"] != step_token_by_step[step_id]:
            _fail("step_token_mismatch", f"/steps/{step_id}/completion_token_id", "step completion token does not name this step")
        for group_id in step["flow_group_ids"]:
            _require_index(group_id, len(groups), f"/steps/{step_id}/flow_group_ids", "flow group")
            if group_id in group_step_membership:
                _fail("duplicate_group_membership", f"/steps/{step_id}/flow_group_ids", "group belongs to more than one step")
            group_step_membership[group_id] = step_id
    if set(group_step_membership) != set(range(len(groups))):
        _fail("missing_group_step_membership", "/steps", "every group must belong to one step")

    epoch_by_group: dict[int, tuple[int, int, int]] = {}
    for program in programs:
        plane_id = program["plane_id"]
        epochs = program["epochs"]
        _require_contiguous_ids(epochs, "program_epoch_id", f"/plane_programs/{plane_id}/epochs")
        previous_configuration: int | None = None
        for epoch in epochs:
            epoch_id = epoch["program_epoch_id"]
            pointer = f"/plane_programs/{plane_id}/epochs/{epoch_id}"
            _require_index(epoch["configuration_id"], len(configurations), f"{pointer}/configuration_id", "configuration")
            _require_sorted_unique(epoch["path_prep_not_before_token_ids"], f"{pointer}/path_prep_not_before_token_ids")
            _require_sorted_unique(epoch["flow_group_ids"], f"{pointer}/flow_group_ids")
            if epoch_id == 0 and epoch["transition"] != "initial":
                _fail("invalid_initial_transition", f"{pointer}/transition", "first used epoch must be initial")
            if epoch_id > 0 and epoch["transition"] == "initial":
                _fail("invalid_later_initial", f"{pointer}/transition", "only epoch zero may be initial")
            if epoch["transition"] == "retain" and epoch["configuration_id"] != previous_configuration:
                _fail("retain_configuration_mismatch", f"{pointer}/configuration_id", "retain must keep the previous configuration")
            if epoch["transition"] == "reconfigure" and epoch["configuration_id"] == previous_configuration:
                _fail("reconfigure_same_configuration", f"{pointer}/configuration_id", "reconfigure must install a different configuration")
            previous_configuration = epoch["configuration_id"]
            for token_id in epoch["path_prep_not_before_token_ids"]:
                _require_index(token_id, len(tokens), f"{pointer}/path_prep_not_before_token_ids", "token")
            for group_id in epoch["flow_group_ids"]:
                _require_index(group_id, len(groups), f"{pointer}/flow_group_ids", "flow group")
                if group_id in epoch_by_group:
                    _fail("duplicate_group_epoch_membership", f"{pointer}/flow_group_ids", "group belongs to more than one plane epoch")
                epoch_by_group[group_id] = (plane_id, epoch_id, epoch["configuration_id"])
    if set(epoch_by_group) != set(range(len(groups))):
        _fail("missing_group_epoch_membership", "/plane_programs", "every group must belong to one plane epoch")

    flow_group_membership: dict[int, int] = {}
    event_edges: dict[str, set[str]] = defaultdict(set)
    for group in groups:
        group_id = group["flow_group_id"]
        pointer = f"/flow_groups/{group_id}"
        if group_step_membership[group_id] != group["step_id"]:
            _fail("group_step_mismatch", f"{pointer}/step_id", "group step cross-check failed")
        expected_epoch = epoch_by_group[group_id]
        actual_epoch = (
            group["plane_id"],
            group["program_epoch_id"],
            group["configuration_id"],
        )
        if actual_epoch != expected_epoch:
            _fail("group_epoch_mismatch", pointer, "group plane/epoch/config cross-check failed")
        if group["completion_token_id"] != group_token_by_group[group_id]:
            _fail("group_token_mismatch", f"{pointer}/completion_token_id", "group completion token does not name this group")
        _require_sorted_unique(group["depends_on_token_ids"], f"{pointer}/depends_on_token_ids")
        _require_sorted_unique(group["flow_ids"], f"{pointer}/flow_ids")
        for token_id in group["depends_on_token_ids"]:
            _require_index(token_id, len(tokens), f"{pointer}/depends_on_token_ids", "token")
            if token_id >= group["completion_token_id"]:
                _fail("token_not_topologically_numbered", f"{pointer}/depends_on_token_ids", "dependency token must precede produced token")
            event_edges[f"token:{token_id}"].add(f"group_release:{group_id}")
        event_edges[
            f"epoch_path:{group['plane_id']}:{group['program_epoch_id']}"
        ].add(f"group_release:{group_id}")
        event_edges[f"group_release:{group_id}"].add(f"group_complete:{group_id}")
        event_edges[f"group_complete:{group_id}"].add(f"token:{group['completion_token_id']}")
        event_edges[f"group_complete:{group_id}"].add(f"step_complete:{group['step_id']}")
        event_edges[f"group_complete:{group_id}"].add(f"epoch_close:{group['plane_id']}:{group['program_epoch_id']}")
        for flow_id in group["flow_ids"]:
            _require_index(flow_id, len(flows), f"{pointer}/flow_ids", "flow")
            if flow_id in flow_group_membership:
                _fail("duplicate_flow_membership", f"{pointer}/flow_ids", "flow belongs to more than one group")
            flow_group_membership[flow_id] = group_id
    if set(flow_group_membership) != set(range(len(flows))):
        _fail("missing_flow_group_membership", "/flow_groups", "every flow must belong to one group")

    for step in steps:
        step_id = step["step_id"]
        event_edges[f"step_complete:{step_id}"].add(f"token:{step['completion_token_id']}")

    for program in programs:
        plane_id = program["plane_id"]
        for epoch in program["epochs"]:
            epoch_id = epoch["program_epoch_id"]
            if epoch_id > 0:
                event_edges[f"epoch_close:{plane_id}:{epoch_id - 1}"].add(f"epoch_path:{plane_id}:{epoch_id}")
            for token_id in epoch["path_prep_not_before_token_ids"]:
                event_edges[f"token:{token_id}"].add(f"epoch_path:{plane_id}:{epoch_id}")

    for flow in flows:
        flow_id = flow["flow_id"]
        pointer = f"/flows/{flow_id}"
        group_id = flow_group_membership[flow_id]
        if flow["flow_group_id"] != group_id:
            _fail("flow_group_mismatch", f"{pointer}/flow_group_id", "flow group cross-check failed")
        if flow["src_rank"] >= node_count or flow["dst_rank"] >= node_count:
            _fail("rank_out_of_range", pointer, "flow endpoint is outside topology")
        if flow["src_rank"] == flow["dst_rank"]:
            _fail("self_flow", f"{pointer}/dst_rank", "active network flow cannot be self traffic")
        group = groups[group_id]
        configuration = configurations[group["configuration_id"]]
        if configuration["permutation"][flow["src_rank"]] != flow["dst_rank"]:
            _fail("flow_route_mismatch", pointer, "flow destination does not match its configuration")
        slice_total = 0
        for slice_index, segment_slice in enumerate(flow["segment_slices"]):
            segment_id = segment_slice["segment_id"]
            _require_index(segment_id, len(segments), f"{pointer}/segment_slices/{slice_index}/segment_id", "segment")
            segment = segments[segment_id]
            offset = segment_slice["segment_offset_bytes"]
            length = segment_slice["length_bytes"]
            if offset > segment["length_bytes"] or length > segment["length_bytes"] - offset:
                _fail("segment_slice_out_of_range", f"{pointer}/segment_slices/{slice_index}", "slice exceeds referenced segment")
            slice_total = _checked_sum((slice_total, length), f"{pointer}/segment_slices")
        if slice_total != flow["payload_bytes"]:
            _fail("flow_slice_byte_mismatch", f"{pointer}/segment_slices", "slice lengths must equal flow payload")

    for group in groups:
        event_edges.setdefault(f"token:{group['completion_token_id']}", set())
    _assert_acyclic(event_edges, "/")

    dependency_mode = plan["dependency_mode"]
    for group in groups:
        step_id = group["step_id"]
        dependencies = group["depends_on_token_ids"]
        if dependency_mode == "global_step_barrier":
            expected = [0] if step_id == 0 else [steps[step_id - 1]["completion_token_id"]]
            if dependencies != expected:
                _fail("global_barrier_dependency_mismatch", f"/flow_groups/{group['flow_group_id']}/depends_on_token_ids", "global barrier group has the wrong release token")

    policy = plan["path_preparation_policy"]
    if policy == "overlap_earliest":
        for program in programs:
            for epoch in program["epochs"]:
                if epoch["path_prep_not_before_token_ids"]:
                    _fail("swot_path_prep_not_earliest", f"/plane_programs/{program['plane_id']}/epochs/{epoch['program_epoch_id']}/path_prep_not_before_token_ids", "SWOT path preparation must not wait for data tokens")
    elif policy == "static_preinstalled":
        for program in programs:
            if program["epochs"] and (
                len(program["epochs"]) != 1
                or program["epochs"][0]["transition"] != "initial"
                or program["epochs"][0]["path_prep_not_before_token_ids"]
            ):
                _fail("one_shot_not_static", f"/plane_programs/{program['plane_id']}/epochs", "one-shot used plane must contain one preinstalled epoch")
    else:
        for program in programs:
            plane_id = program["plane_id"]
            for epoch in program["epochs"]:
                group_steps = {groups[group_id]["step_id"] for group_id in epoch["flow_group_ids"]}
                if len(group_steps) != 1:
                    _fail("baseline_epoch_mixed_steps", f"/plane_programs/{plane_id}/epochs/{epoch['program_epoch_id']}/flow_group_ids", "baseline epoch can contain only one step")
                step_id = next(iter(group_steps))
                expected = [] if step_id == 0 else [steps[step_id - 1]["completion_token_id"]]
                if epoch["path_prep_not_before_token_ids"] != expected:
                    _fail("baseline_path_prep_too_early", f"/plane_programs/{plane_id}/epochs/{epoch['program_epoch_id']}/path_prep_not_before_token_ids", "baseline path preparation must wait for the previous step")
                if epoch["program_epoch_id"] == 0 and step_id != 0:
                    _fail("baseline_late_step_preinstalled", f"/plane_programs/{plane_id}/epochs/0", "baseline cannot preinstall a later step")

    certificate = plan["planner_certificate"]
    nominal_schedule = certificate["nominal_schedule"]
    schedule_ids = [entry["flow_group_id"] for entry in nominal_schedule]
    if schedule_ids != sorted(set(schedule_ids)):
        _fail("nominal_schedule_order", "/planner_certificate/nominal_schedule", "nominal schedule must be sorted and unique")
    for index, entry in enumerate(nominal_schedule):
        _require_index(
            entry["flow_group_id"],
            len(groups),
            f"/planner_certificate/nominal_schedule/{index}/flow_group_id",
            "flow group",
        )
        if entry["planned_release_ps"] > entry["planned_complete_ps"]:
            _fail(
                "nominal_schedule_timing_order",
                f"/planner_certificate/nominal_schedule/{index}",
                "planned release must not follow planned completion",
            )
    if certificate["planner_name"] == "hand_authored_fixture":
        required = (
            certificate["solver_name"] is None
            and certificate["solver_status"] == "not_used"
            and certificate["decision_sha256"] is None
            and certificate["nominal_schedule"] == []
        )
        if not required:
            _fail("fixture_certificate_mismatch", "/planner_certificate", "hand-authored fixture certificate has production fields")
    if certificate["solver_status"] == "optimal" and certificate["relative_gap_ppm"] != 0:
        _fail("optimal_gap_nonzero", "/planner_certificate/relative_gap_ppm", "optimal solver gap must be zero")

    provenance = plan["provenance"]
    provenance_fields = [
        "overlap4ocs_git_sha",
        "htsim_upstream_git_sha",
        "htsim_local_patchset_sha256",
        "instance_file_sha256",
        "program_file_sha256",
        "collective_ir_sha256",
        "plan_build_context_sha256",
    ]
    values = [provenance[field] for field in provenance_fields]
    if provenance["source_kind"] == "hand_authored_fixture" and any(value is not None for value in values):
        _fail("fixture_provenance_nonnull", "/provenance", "fixture provenance hashes must be null")
    if provenance["source_kind"] == "production" and any(value is None for value in values):
        _fail("production_provenance_missing", "/provenance", "production provenance hashes are required")
    if provenance["source_kind"] == "production":
        if certificate["decision_sha256"] is None:
            _fail(
                "production_decision_hash_missing",
                "/planner_certificate/decision_sha256",
                "production plan requires the canonical decision digest",
            )
        if schedule_ids != list(range(len(groups))):
            _fail(
                "production_schedule_incomplete",
                "/planner_certificate/nominal_schedule",
                "production schedule must contain every flow group",
            )
        if certificate["solver_status"] == "not_used":
            if certificate["solver_name"] is not None:
                _fail(
                    "unused_solver_named",
                    "/planner_certificate/solver_name",
                    "solver_name must be null when no solver was used",
                )
        elif certificate["solver_name"] is None or certificate["objective_ps"] is None:
            _fail(
                "solver_certificate_incomplete",
                "/planner_certificate",
                "solver-backed plan requires solver_name and objective_ps",
            )
        if nominal_schedule and certificate["solver_status"] != "not_used":
            planned_complete = max(
                entry["planned_complete_ps"] for entry in nominal_schedule
            )
            if certificate["objective_ps"] != planned_complete:
                _fail(
                    "objective_schedule_mismatch",
                    "/planner_certificate/objective_ps",
                    "solver objective must equal maximum planned completion",
                )

    _checked_sum((flow["payload_bytes"] for flow in flows), "/flows")


ERROR_CODE_RULES: dict[str, set[str]] = {
    "invalid_input": {
        "duplicate_object_key",
        "invalid_json",
        "invalid_utf8",
        "non_finite_number",
        "non_integer_number",
        "plan_file_size_limit",
        "schema_validation_error",
        "uint64_out_of_range",
        "utf8_bom",
    },
    "unsupported_semantics": {
        "result_file_size_limit",
        "unsupported_execution_mode",
        "upstream_pipe_capacity_limit",
    },
    "deadlock": {"no_progress"},
    "invariant_violation": {
        "byte_mismatch",
        "dependency_violation",
        "drop_in_lossless_mode",
        "epoch_violation",
        "route_mismatch",
        "stale_physical_generation",
    },
    "max_simulation_time": {"max_simulation_time_reached"},
    "max_event_count": {"max_event_count_reached"},
    "io_error": {
        "input_changed_during_read",
        "input_not_regular_or_readable",
        "output_commit_failed",
    },
    "internal_error": {"internal_error", "result_size_estimate_violation"},
}


def _interval_union_length(intervals: list[tuple[int, int]]) -> int:
    if not intervals:
        return 0
    merged_start, merged_end = intervals[0]
    total = 0
    for start, end in intervals[1:]:
        if start <= merged_end:
            merged_end = max(merged_end, end)
        else:
            total += merged_end - merged_start
            merged_start, merged_end = start, end
    return total + merged_end - merged_start


def validate_simulation_result(result: dict[str, Any]) -> None:
    """Validate Result v2 status/nullability and aggregate/detail integrity."""

    _schema(result, RESULT_SCHEMA_ID)
    status = result["status"]
    timing = result["timing"]
    traffic = result["traffic"]
    error = result["error"]
    invariants = result["invariants"]

    if result["strategy"] is not None and result["path_preparation_policy"] != (
        _policy_for_strategy(result["strategy"])
    ):
        _fail(
            "strategy_policy_mismatch",
            "/path_preparation_policy",
            "result strategy and path preparation policy do not match",
        )

    for items, key, pointer in (
        (result["tokens"], "token_id", "/tokens"),
        (result["steps"], "step_id", "/steps"),
        (result["flow_groups"], "flow_group_id", "/flow_groups"),
        (result["flows"], "flow_id", "/flows"),
        (result["planes"], "plane_id", "/planes"),
    ):
        _require_contiguous_ids(items, key, pointer)

    if status == "success":
        required_nonnull = (
            "case_id",
            "strategy",
            "plan_file_sha256",
            "units",
            "transport_mode",
            "wire_model",
            "completion_semantics",
            "execution_mode",
            "run_limits",
            "dependency_mode",
            "path_preparation_policy",
            "traffic",
            "provenance",
        )
        if any(result[field] is None for field in required_nonnull):
            _fail("success_missing_identity", "/", "success result has null mandatory identity")
        if result["stop_reason"] != "collective_complete" or error is not None or result["blocked_state"] is not None:
            _fail("success_status_mismatch", "/status", "success status/stop/error/blocked combination is invalid")
        if any(timing[field] is None for field in timing):
            _fail("success_missing_timing", "/timing", "success timing must be complete")
        if timing["collective_start_ps"] != 0:
            _fail("collective_start_not_zero", "/timing/collective_start_ps", "collective starts at zero")
        if timing["simulated_cct_ps"] != timing["collective_complete_ps"]:
            _fail("cct_mismatch", "/timing/simulated_cct_ps", "CCT must equal complete-start")
        if timing["simulation_stop_ps"] != timing["collective_complete_ps"]:
            _fail("simulation_stop_mismatch", "/timing/simulation_stop_ps", "success stops at collective completion")
        if any(invariants[name] is not True for name in MANDATORY_INVARIANTS):
            _fail("success_invariant_not_true", "/invariants", "all mandatory invariants must be true")
    else:
        if result["stop_reason"] == "collective_complete" or error is None:
            _fail("failure_status_mismatch", "/status", "failure requires non-success stop reason and error")
        if timing["simulated_cct_ps"] is not None or timing["collective_complete_ps"] is not None:
            _fail("failure_has_cct", "/timing", "failure CCT and completion must be null")
        allowed = ERROR_CODE_RULES.get(result["stop_reason"], set())
        if error["error_code"] not in allowed:
            _fail("error_code_stop_reason_mismatch", "/error/error_code", "error code is not allowed for stop reason")
        if result["stop_reason"] in {"deadlock", "invariant_violation", "max_simulation_time", "max_event_count"} and result["blocked_state"] is None:
            _fail("runtime_failure_missing_blocked_state", "/blocked_state", "runtime failure requires blocked state")

    plan_identity_fields = (
        "case_id",
        "strategy",
        "units",
        "transport_mode",
        "wire_model",
        "completion_semantics",
        "execution_mode",
        "run_limits",
        "dependency_mode",
        "path_preparation_policy",
        "provenance",
    )
    if traffic is not None:
        if any(result[field] is None for field in plan_identity_fields):
            _fail(
                "runtime_result_missing_identity",
                "/",
                "result with runtime traffic requires all plan-derived identity",
            )
        if timing["collective_start_ps"] != 0 or timing["simulation_stop_ps"] is None:
            _fail(
                "runtime_result_missing_timing",
                "/timing",
                "runtime result requires start=0 and a simulation stop time",
            )
    elif result["case_id"] is None:
        for field in plan_identity_fields:
            if result[field] is not None:
                _fail(
                    "untrusted_result_has_identity",
                    f"/{field}",
                    "untrusted input failure cannot carry plan-derived identity",
                )
    elif result["plan_file_sha256"] is None or any(
        result[field] is None for field in plan_identity_fields
    ):
        _fail(
            "trusted_result_missing_identity",
            "/",
            "trusted pre-execution failure requires all plan-derived identity",
        )

    if error is not None:
        entity_type = error["entity_type"]
        entity_id = error["entity_id"]
        plane_id = error["plane_id"]
        if entity_type == "none" and (entity_id is not None or plane_id is not None):
            _fail("error_entity_mismatch", "/error", "global error cannot carry entity IDs")
        if entity_type in {"step", "flow_group", "flow"} and (entity_id is None or plane_id is not None):
            _fail("error_entity_mismatch", "/error", "entity error requires only entity_id")
        if entity_type == "plane_epoch" and (entity_id is None or plane_id is None):
            _fail("error_entity_mismatch", "/error", "plane_epoch error requires plane and epoch IDs")

    if traffic is None:
        if any(result[name] for name in ("tokens", "steps", "flow_groups", "flows", "planes")):
            _fail("preexecution_result_has_details", "/traffic", "result without traffic cannot contain runtime details")
        return

    for token in result["tokens"]:
        token_id = token["token_id"]
        if (token["status"] == "ready") != (token["ready_ps"] is not None):
            _fail(
                "token_status_timing_mismatch",
                f"/tokens/{token_id}",
                "ready token requires ready_ps and pending token forbids it",
            )

    flows = result["flows"]
    groups = result["flow_groups"]
    steps = result["steps"]
    planes = result["planes"]
    completed_flows = sum(flow["status"] == "complete" for flow in flows)
    completed_groups = sum(group["status"] == "complete" for group in groups)
    expected_payload = _checked_sum((flow["payload_bytes"] for flow in flows), "/flows")
    sent_payload = _checked_sum((flow["sent_payload_bytes"] for flow in flows), "/flows")
    received_payload = _checked_sum((flow["received_payload_bytes"] for flow in flows), "/flows")
    aggregate_checks = {
        "expected_flow_count": len(flows),
        "completed_flow_count": completed_flows,
        "expected_flow_group_count": len(groups),
        "completed_flow_group_count": completed_groups,
        "expected_payload_bytes": expected_payload,
        "sent_payload_bytes": sent_payload,
        "received_payload_bytes": received_payload,
        "missing_payload_bytes": expected_payload - received_payload,
    }
    for field, expected in aggregate_checks.items():
        if traffic[field] != expected:
            _fail("result_aggregate_mismatch", f"/traffic/{field}", f"expected {expected}")
    unfinished = [flow["flow_id"] for flow in flows if flow["status"] != "complete"]
    if traffic["unfinished_flow_ids"] != unfinished:
        _fail("unfinished_flow_ids_mismatch", "/traffic/unfinished_flow_ids", "unfinished flow IDs do not match details")

    for flow in flows:
        flow_id = flow["flow_id"]
        _require_index(
            flow["flow_group_id"],
            len(groups),
            f"/flows/{flow_id}/flow_group_id",
            "flow group",
        )
        if flow["sent_payload_bytes"] > flow["payload_bytes"] or flow["received_payload_bytes"] > flow["sent_payload_bytes"]:
            _fail("flow_counter_order", f"/flows/{flow_id}", "flow counters exceed payload or sent bytes")
        if flow["status"] == "complete" and (
            flow["sent_payload_bytes"] != flow["payload_bytes"]
            or flow["received_payload_bytes"] != flow["payload_bytes"]
            or flow["last_payload_received_ps"] is None
        ):
            _fail("complete_flow_counter_mismatch", f"/flows/{flow_id}", "complete flow must contain exact bytes and completion time")
        if flow["status"] == "complete" and not (
            flow["release_ps"]
            <= flow["last_payload_sent_ps"]
            <= flow["last_payload_received_ps"]
        ):
            _fail(
                "flow_timing_order",
                f"/flows/{flow_id}",
                "complete flow timing must be release <= last-sent <= last-received",
            )

    flow_membership: dict[int, int] = {}

    for group in groups:
        group_id = group["flow_group_id"]
        _require_sorted_unique(group["flow_ids"], f"/flow_groups/{group_id}/flow_ids")
        for flow_id in group["flow_ids"]:
            _require_index(
                flow_id,
                len(flows),
                f"/flow_groups/{group_id}/flow_ids",
                "flow",
            )
            if flow_id in flow_membership:
                _fail(
                    "duplicate_flow_membership",
                    f"/flow_groups/{group_id}/flow_ids",
                    "result flow belongs to more than one group",
                )
            flow_membership[flow_id] = group_id
        member_flows = [flows[flow_id] for flow_id in group["flow_ids"]]
        expected_group = {
            "expected_flow_count": len(member_flows),
            "completed_flow_count": sum(flow["status"] == "complete" for flow in member_flows),
            "expected_payload_bytes": _checked_sum((flow["payload_bytes"] for flow in member_flows), f"/flow_groups/{group_id}"),
            "sent_payload_bytes": _checked_sum((flow["sent_payload_bytes"] for flow in member_flows), f"/flow_groups/{group_id}"),
            "received_payload_bytes": _checked_sum((flow["received_payload_bytes"] for flow in member_flows), f"/flow_groups/{group_id}"),
        }
        for field, expected in expected_group.items():
            if group[field] != expected:
                _fail("group_aggregate_mismatch", f"/flow_groups/{group_id}/{field}", f"expected {expected}")
        if group["status"] == "complete":
            times = (
                group["dependency_ready_ps"],
                group["release_ps"],
                group["completion_ps"],
            )
            if group["completed_flow_count"] != group["expected_flow_count"] or any(
                value is None for value in times
            ):
                _fail(
                    "complete_group_state_mismatch",
                    f"/flow_groups/{group_id}",
                    "complete group needs all flows and complete timing",
                )
            dependency_ready, release, completion = times
            assert dependency_ready is not None and release is not None and completion is not None
            if not dependency_ready <= release <= completion:
                _fail(
                    "group_timing_order",
                    f"/flow_groups/{group_id}",
                    "group timing must be dependency-ready <= release <= completion",
                )
            expected_completion = max(
                flow["last_payload_received_ps"] for flow in member_flows
            )
            if completion != expected_completion:
                _fail(
                    "group_completion_timing_mismatch",
                    f"/flow_groups/{group_id}/completion_ps",
                    "group completion must equal its last completed flow",
                )
    if set(flow_membership) != set(range(len(flows))):
        _fail(
            "missing_flow_group_membership",
            "/flow_groups",
            "every result flow must belong to exactly one group",
        )
    for flow in flows:
        if flow_membership[flow["flow_id"]] != flow["flow_group_id"]:
            _fail(
                "flow_group_mismatch",
                f"/flows/{flow['flow_id']}/flow_group_id",
                "result flow/group membership cross-check failed",
            )

    group_membership: dict[int, int] = {}
    for step in steps:
        step_id = step["step_id"]
        _require_sorted_unique(step["flow_group_ids"], f"/steps/{step_id}/flow_group_ids")
        for group_id in step["flow_group_ids"]:
            _require_index(
                group_id,
                len(groups),
                f"/steps/{step_id}/flow_group_ids",
                "flow group",
            )
            if group_id in group_membership:
                _fail(
                    "duplicate_group_membership",
                    f"/steps/{step_id}/flow_group_ids",
                    "result group belongs to more than one step",
                )
            group_membership[group_id] = step_id
        member_groups = [groups[group_id] for group_id in step["flow_group_ids"]]
        if step["expected_flow_group_count"] != len(member_groups) or step["completed_flow_group_count"] != sum(group["status"] == "complete" for group in member_groups):
            _fail("step_aggregate_mismatch", f"/steps/{step_id}", "step group counts do not match details")
        if step["status"] == "complete":
            if (
                step["completed_flow_group_count"]
                != step["expected_flow_group_count"]
                or step["first_flow_group_release_ps"] is None
                or step["completion_ps"] is None
            ):
                _fail(
                    "complete_step_state_mismatch",
                    f"/steps/{step_id}",
                    "complete step needs all groups and complete timing",
                )
            if step["first_flow_group_release_ps"] > step["completion_ps"]:
                _fail(
                    "step_timing_order",
                    f"/steps/{step_id}",
                    "first release must not follow step completion",
                )
            expected_first_release = min(
                group["release_ps"] for group in member_groups
            )
            expected_completion = max(
                group["completion_ps"] for group in member_groups
            )
            if (
                step["first_flow_group_release_ps"] != expected_first_release
                or step["completion_ps"] != expected_completion
            ):
                _fail(
                    "step_timing_aggregate_mismatch",
                    f"/steps/{step_id}",
                    "step timing must be min release and max group completion",
                )
    if set(group_membership) != set(range(len(groups))):
        _fail(
            "missing_group_step_membership",
            "/steps",
            "every result group must belong to exactly one step",
        )
    for group in groups:
        if group_membership[group["flow_group_id"]] != group["step_id"]:
            _fail(
                "group_step_mismatch",
                f"/flow_groups/{group['flow_group_id']}/step_id",
                "result group/step membership cross-check failed",
            )

    tokens = result["tokens"]
    if not tokens or tokens[0] != {
        "producer_id": None,
        "ready_ps": 0,
        "status": "ready",
        "token_id": 0,
        "token_type": "collective_start",
    }:
        _fail(
            "invalid_collective_start_token",
            "/tokens/0",
            "runtime result requires ready collective-start token 0",
        )
    group_token_by_group: dict[int, dict[str, Any]] = {}
    step_token_by_step: dict[int, dict[str, Any]] = {}
    for token in tokens[1:]:
        producer_id = token["producer_id"]
        if producer_id is None:
            _fail(
                "token_producer_mismatch",
                f"/tokens/{token['token_id']}/producer_id",
                "completion token requires a producer",
            )
        if token["token_type"] == "flow_group_complete":
            _require_index(
                producer_id,
                len(groups),
                f"/tokens/{token['token_id']}/producer_id",
                "flow group",
            )
            if producer_id in group_token_by_group:
                _fail(
                    "duplicate_token_producer",
                    f"/tokens/{token['token_id']}",
                    "result group has duplicate completion tokens",
                )
            group_token_by_group[producer_id] = token
        elif token["token_type"] == "step_complete":
            _require_index(
                producer_id,
                len(steps),
                f"/tokens/{token['token_id']}/producer_id",
                "step",
            )
            if producer_id in step_token_by_step:
                _fail(
                    "duplicate_token_producer",
                    f"/tokens/{token['token_id']}",
                    "result step has duplicate completion tokens",
                )
            step_token_by_step[producer_id] = token
        else:
            _fail(
                "duplicate_collective_start_token",
                f"/tokens/{token['token_id']}",
                "collective-start token may appear only at index zero",
            )
    if set(group_token_by_group) != set(range(len(groups))) or set(
        step_token_by_step
    ) != set(range(len(steps))):
        _fail(
            "missing_completion_token",
            "/tokens",
            "every result group and step requires one completion token",
        )
    for group in groups:
        token = group_token_by_group[group["flow_group_id"]]
        if (group["status"] == "complete") != (token["status"] == "ready"):
            _fail(
                "group_token_status_mismatch",
                f"/tokens/{token['token_id']}",
                "group completion and token readiness differ",
            )
        if token["ready_ps"] != group["completion_ps"]:
            _fail(
                "group_token_timing_mismatch",
                f"/tokens/{token['token_id']}/ready_ps",
                "group token time differs from group completion",
            )
    for step in steps:
        token = step_token_by_step[step["step_id"]]
        if token["token_id"] != step["completion_token_id"]:
            _fail(
                "step_token_mismatch",
                f"/steps/{step['step_id']}/completion_token_id",
                "step completion token ID does not name this step",
            )
        if (step["status"] == "complete") != (token["status"] == "ready"):
            _fail(
                "step_token_status_mismatch",
                f"/tokens/{token['token_id']}",
                "step completion and token readiness differ",
            )
        if token["ready_ps"] != step["completion_ps"]:
            _fail(
                "step_token_timing_mismatch",
                f"/tokens/{token['token_id']}/ready_ps",
                "step token time differs from step completion",
            )

    for group in groups:
        dependency_times = []
        for token_id in group["depends_on_token_ids"]:
            _require_index(
                token_id,
                len(tokens),
                f"/flow_groups/{group['flow_group_id']}/depends_on_token_ids",
                "token",
            )
            dependency_times.append(tokens[token_id]["ready_ps"])
        if all(value is not None for value in dependency_times):
            expected_ready = max(dependency_times)
            if group["dependency_ready_ps"] != expected_ready:
                _fail(
                    "group_dependency_timing_mismatch",
                    f"/flow_groups/{group['flow_group_id']}/dependency_ready_ps",
                    "dependency-ready time differs from input tokens",
                )
        elif group["dependency_ready_ps"] is not None:
            _fail(
                "group_dependency_timing_mismatch",
                f"/flow_groups/{group['flow_group_id']}/dependency_ready_ps",
                "group cannot be dependency-ready while an input token is pending",
            )

    rank_count = len(traffic["per_rank_sent_payload_bytes"])
    if len(traffic["per_rank_received_payload_bytes"]) != rank_count:
        _fail("per_rank_array_length", "/traffic", "per-rank arrays have different lengths")
    if len(traffic["per_plane_payload_bytes"]) != len(planes):
        _fail("per_plane_array_length", "/traffic/per_plane_payload_bytes", "per-plane array length mismatch")
    recomputed_rank_sent = [0] * rank_count
    recomputed_rank_received = [0] * rank_count
    for flow in flows:
        if flow["src_rank"] >= rank_count or flow["dst_rank"] >= rank_count:
            _fail("result_rank_out_of_range", f"/flows/{flow['flow_id']}", "result flow rank exceeds per-rank arrays")
        recomputed_rank_sent[flow["src_rank"]] += flow["sent_payload_bytes"]
        recomputed_rank_received[flow["dst_rank"]] += flow["received_payload_bytes"]
    if recomputed_rank_sent != traffic["per_rank_sent_payload_bytes"] or recomputed_rank_received != traffic["per_rank_received_payload_bytes"]:
        _fail("per_rank_aggregate_mismatch", "/traffic", "per-rank counters do not match flows")

    recomputed_plane = []
    epoch_membership: dict[int, tuple[int, int, int, int | None, int | None]] = {}
    for plane in planes:
        plane_id = plane["plane_id"]
        if len(plane["source_ports"]) != rank_count:
            _fail("source_port_count_mismatch", f"/planes/{plane_id}/source_ports", "one source port is required per rank")
        _require_contiguous_ids(plane["source_ports"], "src_rank", f"/planes/{plane_id}/source_ports")
        all_intervals: list[tuple[int, int]] = []
        for port in plane["source_ports"]:
            previous_end = 0
            interval_length = 0
            for index, interval in enumerate(port["busy_intervals"]):
                start = interval["start_ps"]
                end = interval["end_ps"]
                if end < start or (index and start < previous_end):
                    _fail("busy_interval_order", f"/planes/{plane_id}/source_ports/{port['src_rank']}/busy_intervals/{index}", "busy intervals must be ordered and nonoverlapping")
                if status == "success" and interval["truncated_at_stop"]:
                    _fail("success_truncated_interval", f"/planes/{plane_id}/source_ports/{port['src_rank']}/busy_intervals/{index}", "success cannot contain truncated busy intervals")
                if timing["simulation_stop_ps"] is not None and end > timing[
                    "simulation_stop_ps"
                ]:
                    _fail(
                        "busy_interval_after_stop",
                        f"/planes/{plane_id}/source_ports/{port['src_rank']}/busy_intervals/{index}",
                        "busy interval extends beyond simulation stop",
                    )
                if interval["truncated_at_stop"] and (
                    index != len(port["busy_intervals"]) - 1
                    or end != timing["simulation_stop_ps"]
                ):
                    _fail(
                        "truncated_interval_mismatch",
                        f"/planes/{plane_id}/source_ports/{port['src_rank']}/busy_intervals/{index}",
                        "only the final interval may be truncated at stop",
                    )
                interval_length += end - start
                all_intervals.append((start, end))
                previous_end = end
            if port["busy_time_ps"] != interval_length:
                _fail("source_port_busy_time_mismatch", f"/planes/{plane_id}/source_ports/{port['src_rank']}/busy_time_ps", "source port busy time does not match intervals")
            port_flows = [
                flow
                for flow in flows
                if flow["src_rank"] == port["src_rank"]
                and groups[flow["flow_group_id"]]["plane_id"] == plane_id
            ]
            if port["sent_payload_bytes"] != _checked_sum(
                (flow["sent_payload_bytes"] for flow in port_flows),
                f"/planes/{plane_id}/source_ports/{port['src_rank']}",
            ) or port["logical_packet_count"] != _checked_sum(
                (flow["logical_packet_count"] for flow in port_flows),
                f"/planes/{plane_id}/source_ports/{port['src_rank']}",
            ):
                _fail(
                    "source_port_aggregate_mismatch",
                    f"/planes/{plane_id}/source_ports/{port['src_rank']}",
                    "source port counters do not match its flows",
                )
        all_intervals.sort()
        if plane["busy_time_ps"] != _interval_union_length(all_intervals):
            _fail("plane_busy_time_mismatch", f"/planes/{plane_id}/busy_time_ps", "plane busy time must be interval union length")
        if plane["max_serializer_backlog_bytes"] != max(
            (port["max_backlog_bytes"] for port in plane["source_ports"]),
            default=0,
        ):
            _fail(
                "plane_backlog_mismatch",
                f"/planes/{plane_id}/max_serializer_backlog_bytes",
                "plane backlog must equal the largest source-port backlog",
            )
        plane_flow_groups = [group for group in groups if group["plane_id"] == plane_id]
        expected = _checked_sum((group["expected_payload_bytes"] for group in plane_flow_groups), f"/planes/{plane_id}")
        sent = _checked_sum((group["sent_payload_bytes"] for group in plane_flow_groups), f"/planes/{plane_id}")
        received = _checked_sum((group["received_payload_bytes"] for group in plane_flow_groups), f"/planes/{plane_id}")
        if (plane["expected_payload_bytes"], plane["sent_payload_bytes"], plane["received_payload_bytes"]) != (expected, sent, received):
            _fail("plane_aggregate_mismatch", f"/planes/{plane_id}", "plane byte counters do not match groups")
        recomputed_plane.append(expected)
        _require_contiguous_ids(plane["epochs"], "program_epoch_id", f"/planes/{plane_id}/epochs")
        policy = result["path_preparation_policy"]
        if policy == "static_preinstalled" and plane["epochs"] and (
            len(plane["epochs"]) != 1
            or plane["epochs"][0]["transition"] != "initial"
            or plane["epochs"][0]["path_prep_not_before_token_ids"]
        ):
            _fail(
                "one_shot_not_static",
                f"/planes/{plane_id}/epochs",
                "one-shot result plane must contain one preinstalled epoch",
            )
        generation = 0
        previous_configuration: int | None = None
        previous_close: int | None = None
        for epoch in plane["epochs"]:
            epoch_id = epoch["program_epoch_id"]
            pointer = f"/planes/{plane_id}/epochs/{epoch_id}"
            _require_sorted_unique(
                epoch["path_prep_not_before_token_ids"],
                f"{pointer}/path_prep_not_before_token_ids",
            )
            _require_sorted_unique(
                epoch["flow_group_ids"], f"{pointer}/flow_group_ids"
            )
            if not epoch["flow_group_ids"]:
                _fail(
                    "empty_used_epoch",
                    f"{pointer}/flow_group_ids",
                    "a reported runtime epoch must contain a flow group",
                )
            if policy == "overlap_earliest" and epoch[
                "path_prep_not_before_token_ids"
            ]:
                _fail(
                    "swot_path_prep_not_earliest",
                    f"{pointer}/path_prep_not_before_token_ids",
                    "SWOT path preparation must not wait for data tokens",
                )
            if policy == "step_lockstep":
                group_steps = {
                    groups[group_id]["step_id"]
                    for group_id in epoch["flow_group_ids"]
                    if group_id < len(groups)
                }
                if len(group_steps) != 1:
                    _fail(
                        "baseline_epoch_mixed_steps",
                        f"{pointer}/flow_group_ids",
                        "baseline result epoch can contain only one step",
                    )
                step_id = next(iter(group_steps))
                expected_gate = (
                    []
                    if step_id == 0
                    else [step_token_by_step[step_id - 1]["token_id"]]
                )
                if epoch["path_prep_not_before_token_ids"] != expected_gate:
                    _fail(
                        "baseline_path_prep_too_early",
                        f"{pointer}/path_prep_not_before_token_ids",
                        "baseline result path gate differs from previous step token",
                    )
                if epoch_id == 0 and step_id != 0:
                    _fail(
                        "baseline_late_step_preinstalled",
                        pointer,
                        "baseline cannot preinstall a later step",
                    )
            if epoch_id == 0 and epoch["transition"] != "initial":
                _fail(
                    "invalid_initial_transition",
                    f"{pointer}/transition",
                    "first result epoch must be initial",
                )
            if epoch_id > 0 and epoch["transition"] == "initial":
                _fail(
                    "invalid_later_initial",
                    f"{pointer}/transition",
                    "only result epoch zero may be initial",
                )
            if epoch["transition"] == "reconfigure":
                generation += 1
                if epoch["reconfiguration_start_ps"] is None or epoch["reconfiguration_end_ps"] is None:
                    _fail("reconfiguration_timing_missing", pointer, "reconfigure epoch needs start/end")
            elif epoch["reconfiguration_start_ps"] is not None or epoch["reconfiguration_end_ps"] is not None:
                _fail("unexpected_reconfiguration_timing", pointer, "initial/retain cannot report reconfiguration timing")
            if epoch["physical_config_generation"] is not None and epoch["physical_config_generation"] != generation:
                _fail("physical_generation_mismatch", f"{pointer}/physical_config_generation", "physical generation does not follow transitions")
            if epoch["transition"] == "retain" and previous_configuration is not None and epoch["configuration_id"] != previous_configuration:
                _fail("retain_configuration_mismatch", f"{pointer}/configuration_id", "retain changed configuration")
            if epoch["transition"] == "reconfigure" and epoch["configuration_id"] == previous_configuration:
                _fail(
                    "reconfigure_same_configuration",
                    f"{pointer}/configuration_id",
                    "reconfigure must install a different configuration",
                )
            previous_configuration = epoch["configuration_id"]
            path_start = epoch["path_prep_start_ps"]
            gate_times: list[int] = []
            if epoch_id > 0:
                if path_start is not None and previous_close is None:
                    _fail(
                        "epoch_program_order",
                        f"{pointer}/path_prep_start_ps",
                        "later epoch cannot start before the prior epoch closes",
                    )
                if previous_close is not None:
                    gate_times.append(previous_close)
            for token_id in epoch["path_prep_not_before_token_ids"]:
                _require_index(
                    token_id,
                    len(tokens),
                    f"{pointer}/path_prep_not_before_token_ids",
                    "token",
                )
                ready_ps = tokens[token_id]["ready_ps"]
                if path_start is not None and ready_ps is None:
                    _fail(
                        "epoch_path_gate_violation",
                        f"{pointer}/path_prep_start_ps",
                        "path preparation started before a gate token was ready",
                    )
                if ready_ps is not None:
                    gate_times.append(ready_ps)
            if path_start is not None and gate_times and path_start < max(gate_times):
                _fail(
                    "epoch_path_gate_violation",
                    f"{pointer}/path_prep_start_ps",
                    "path preparation started before all guards",
                )
            if (
                status == "success"
                and policy == "overlap_earliest"
                and epoch_id > 0
                and path_start != previous_close
            ):
                _fail(
                    "swot_path_prep_not_earliest",
                    f"{pointer}/path_prep_start_ps",
                    "SWOT path preparation must start when the prior epoch closes",
                )
            for group_id in epoch["flow_group_ids"]:
                _require_index(
                    group_id,
                    len(groups),
                    f"{pointer}/flow_group_ids",
                    "flow group",
                )
                if group_id in epoch_membership:
                    _fail(
                        "duplicate_group_epoch_membership",
                        f"{pointer}/flow_group_ids",
                        "result group belongs to more than one epoch",
                    )
                epoch_membership[group_id] = (
                    plane_id,
                    epoch_id,
                    epoch["configuration_id"],
                    epoch["physical_config_generation"],
                    epoch["path_ready_ps"],
                )
            if status == "success" and epoch["flow_group_ids"]:
                required_times = (
                    "path_prep_start_ps",
                    "path_ready_ps",
                    "transfer_start_ps",
                    "transfer_complete_ps",
                    "drain_complete_ps",
                    "close_ps",
                )
                if epoch["status"] != "closed" or any(
                    epoch[field] is None for field in required_times
                ):
                    _fail(
                        "success_epoch_incomplete",
                        pointer,
                        "success epoch must be closed with complete timing",
                    )
                path_ready = epoch["path_ready_ps"]
                transfer_start = epoch["transfer_start_ps"]
                transfer_complete = epoch["transfer_complete_ps"]
                drain_complete = epoch["drain_complete_ps"]
                close = epoch["close_ps"]
                assert all(
                    value is not None
                    for value in (
                        path_start,
                        path_ready,
                        transfer_start,
                        transfer_complete,
                        drain_complete,
                        close,
                    )
                )
                if not (
                    path_start
                    <= path_ready
                    <= transfer_start
                    <= transfer_complete
                    <= drain_complete
                    <= close
                ):
                    _fail(
                        "epoch_timing_order",
                        pointer,
                        "epoch timing does not follow path/transfer/drain/close order",
                    )
                if epoch["transition"] == "initial" and (
                    path_start != 0 or path_ready != 0
                ):
                    _fail(
                        "initial_epoch_timing",
                        pointer,
                        "initial path is preinstalled at time zero",
                    )
                if epoch["transition"] == "retain" and path_start != path_ready:
                    _fail(
                        "retain_epoch_timing",
                        pointer,
                        "retain has zero path preparation duration",
                    )
                if epoch["transition"] == "reconfigure" and (
                    epoch["reconfiguration_start_ps"] != path_start
                    or epoch["reconfiguration_end_ps"] != path_ready
                ):
                    _fail(
                        "reconfiguration_timing_mismatch",
                        pointer,
                        "reconfiguration start/end must equal path start/ready",
                    )
            if epoch["close_ps"] is not None:
                previous_close = epoch["close_ps"]
        if plane["max_serializer_backlog_bytes"] != max(
            (epoch["max_serializer_backlog_bytes"] for epoch in plane["epochs"]),
            default=0,
        ):
            _fail(
                "plane_epoch_backlog_mismatch",
                f"/planes/{plane_id}/max_serializer_backlog_bytes",
                "plane backlog must equal the largest epoch backlog",
            )
    if recomputed_plane != traffic["per_plane_payload_bytes"]:
        _fail("per_plane_aggregate_mismatch", "/traffic/per_plane_payload_bytes", "per-plane payload totals do not match plane details")
    if set(epoch_membership) != set(range(len(groups))):
        _fail(
            "missing_group_epoch_membership",
            "/planes",
            "every result group must belong to exactly one plane epoch",
        )
    if result["path_preparation_policy"] == "step_lockstep":
        for step in steps:
            member_groups = [groups[group_id] for group_id in step["flow_group_ids"]]
            released = [
                group["release_ps"]
                for group in member_groups
                if group["release_ps"] is not None
            ]
            if released:
                path_ready_times = [
                    epoch_membership[group["flow_group_id"]][4]
                    for group in member_groups
                ]
                if any(value is None for value in path_ready_times) or min(
                    released
                ) < max(value for value in path_ready_times if value is not None):
                    _fail(
                        "baseline_all_paths_gate_violation",
                        f"/steps/{step['step_id']}",
                        "baseline group released before every step path was ready",
                    )
    for group in groups:
        group_id = group["flow_group_id"]
        plane_id, epoch_id, configuration_id, generation, path_ready = (
            epoch_membership[group_id]
        )
        if (
            group["plane_id"],
            group["program_epoch_id"],
            group["configuration_id"],
        ) != (plane_id, epoch_id, configuration_id) or (
            group["physical_config_generation"] is not None
            and group["physical_config_generation"] != generation
        ):
            _fail(
                "group_epoch_mismatch",
                f"/flow_groups/{group_id}",
                "result group plane/epoch/config/generation cross-check failed",
            )
        if group["release_ps"] is not None and (
            group["dependency_ready_ps"] is None
            or path_ready is None
            or group["release_ps"] < max(group["dependency_ready_ps"], path_ready)
        ):
            _fail(
                "group_release_guard_violation",
                f"/flow_groups/{group_id}/release_ps",
                "group released before dependencies and path were ready",
            )

    if traffic["logical_packet_count"] != _checked_sum(
        (flow["logical_packet_count"] for flow in flows), "/traffic"
    ):
        _fail(
            "logical_packet_aggregate_mismatch",
            "/traffic/logical_packet_count",
            "logical packet total does not match flows",
        )
    if result["wire_model"] == "payload_only" and traffic["wire_bytes"] != sent_payload:
        _fail(
            "wire_byte_aggregate_mismatch",
            "/traffic/wire_bytes",
            "payload-only wire bytes must equal sent payload bytes",
        )
    if (
        result["execution_mode"] == "full_packet"
        and traffic["simulated_transit_unit_count"]
        != traffic["logical_packet_count"]
    ):
        _fail(
            "transit_unit_aggregate_mismatch",
            "/traffic/simulated_transit_unit_count",
            "full-packet transit-unit count must equal logical packet count",
        )

    if status == "success":
        zero_fields = (
            "drops",
            "duplicate_payload_bytes",
            "missing_payload_bytes",
            "in_flight_transit_unit_count",
        )
        if any(traffic[field] != 0 for field in zero_fields) or traffic[
            "unfinished_flow_ids"
        ]:
            _fail(
                "success_traffic_not_complete",
                "/traffic",
                "success traffic has loss, duplicates, missing, in-flight, or unfinished data",
            )
        if (
            any(flow["status"] != "complete" for flow in flows)
            or any(group["status"] != "complete" for group in groups)
            or any(step["status"] != "complete" for step in steps)
        ):
            _fail(
                "success_detail_not_complete",
                "/",
                "all flow/group/step details must be complete",
            )
        provenance = result["provenance"]
        assert provenance is not None
        if result["execution_mode"] == "exact_coalesced" and provenance["exact_coalesced_equivalence_version"] is None:
            _fail("coalesced_equivalence_missing", "/provenance/exact_coalesced_equivalence_version", "coalesced success needs equivalence version")
        if result["execution_mode"] == "full_packet" and provenance["exact_coalesced_equivalence_version"] is not None:
            _fail("unexpected_coalesced_equivalence", "/provenance/exact_coalesced_equivalence_version", "full_packet must not name coalesced equivalence")


def _safe_relative_path(value: str, pointer: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        _fail("invalid_relative_path", pointer, "path must be normalized and relative")
    return path


def _resolve_pointer(document: Any, pointer: str) -> Any:
    current = document
    if pointer == "":
        return current
    for encoded in pointer[1:].split("/"):
        component = encoded.replace("~1", "/").replace("~0", "~")
        if isinstance(current, list):
            try:
                current = current[int(component, 10)]
            except (ValueError, IndexError) as error:
                raise KeyError(pointer) from error
        elif isinstance(current, dict) and component in current:
            current = current[component]
        else:
            raise KeyError(pointer)
    return current


def _require_plan_result_echo(
    plan: dict[str, Any], result: dict[str, Any]
) -> None:
    """Cross-check all plan-owned fields repeated by a trusted result."""

    if result["traffic"] is None:
        return
    collection_pairs = (
        ("readiness_tokens", "tokens"),
        ("steps", "steps"),
        ("flow_groups", "flow_groups"),
        ("flows", "flows"),
        ("plane_programs", "planes"),
    )
    for plan_name, result_name in collection_pairs:
        if len(plan[plan_name]) != len(result[result_name]):
            _fail(
                "result_plan_collection_mismatch",
                f"/{result_name}",
                f"result {result_name} length differs from plan {plan_name}",
            )

    shared_fields = {
        "readiness_tokens": ("token_id", "token_type", "producer_id"),
        "steps": ("step_id", "flow_group_ids", "completion_token_id"),
        "flow_groups": (
            "flow_group_id",
            "step_id",
            "plane_id",
            "program_epoch_id",
            "configuration_id",
            "depends_on_token_ids",
            "flow_ids",
        ),
        "flows": (
            "flow_id",
            "flow_group_id",
            "src_rank",
            "dst_rank",
            "payload_bytes",
        ),
    }
    result_collection = {
        "readiness_tokens": "tokens",
        "steps": "steps",
        "flow_groups": "flow_groups",
        "flows": "flows",
    }
    for plan_name, fields in shared_fields.items():
        result_name = result_collection[plan_name]
        for index, (planned, observed) in enumerate(
            zip(plan[plan_name], result[result_name])
        ):
            for field in fields:
                if planned[field] != observed[field]:
                    _fail(
                        "result_plan_field_mismatch",
                        f"/{result_name}/{index}/{field}",
                        f"result does not echo plan field {field}",
                    )

    for plane_id, (program, plane) in enumerate(
        zip(plan["plane_programs"], result["planes"])
    ):
        if program["plane_id"] != plane["plane_id"]:
            _fail(
                "result_plan_field_mismatch",
                f"/planes/{plane_id}/plane_id",
                "result plane ID differs from plan",
            )
        if len(program["epochs"]) != len(plane["epochs"]):
            _fail(
                "result_plan_collection_mismatch",
                f"/planes/{plane_id}/epochs",
                "result epoch count differs from plan",
            )
        for epoch_id, (planned, observed) in enumerate(
            zip(program["epochs"], plane["epochs"])
        ):
            for field in (
                "program_epoch_id",
                "transition",
                "configuration_id",
                "path_prep_not_before_token_ids",
                "flow_group_ids",
            ):
                if planned[field] != observed[field]:
                    _fail(
                        "result_plan_field_mismatch",
                        f"/planes/{plane_id}/epochs/{epoch_id}/{field}",
                        f"result does not echo plan epoch field {field}",
                    )
            if planned["transition"] == "reconfigure":
                start = observed["reconfiguration_start_ps"]
                end = observed["reconfiguration_end_ps"]
                delay = plan["topology"]["reconfiguration_delay_ps"]
                if start is not None and (
                    start > UINT64_MAX - delay or end != start + delay
                ):
                    _fail(
                        "reconfiguration_delay_mismatch",
                        f"/planes/{plane_id}/epochs/{epoch_id}/reconfiguration_end_ps",
                        "reconfiguration duration differs from plan topology",
                    )

    provenance = result["provenance"]
    if provenance is not None:
        expected_schema_hashes = {
            "plan_schema_sha256": file_sha256(SCHEMA_FILES[PLAN_SCHEMA_ID]),
            "result_schema_sha256": file_sha256(SCHEMA_FILES[RESULT_SCHEMA_ID]),
        }
        for field, expected in expected_schema_hashes.items():
            if provenance[field] != expected:
                _fail(
                    "result_schema_hash_mismatch",
                    f"/provenance/{field}",
                    "result schema raw digest differs from local contract",
                )


def validate_contract_pair(
    pair: dict[str, Any], fixture_root: Path | None = None
) -> None:
    _schema(pair, PAIR_SCHEMA_ID)
    checks = pair["equality_checks"]
    sort_keys = [
        (check["plan_json_pointer"], check["result_json_pointer"])
        for check in checks
    ]
    if sort_keys != sorted(set(sort_keys)):
        _fail("pair_check_order", "/equality_checks", "equality checks must be sorted and unique")
    plan_relative = _safe_relative_path(pair["plan_path"], "/plan_path")
    result_relative = _safe_relative_path(pair["result_path"], "/result_path")
    if fixture_root is None:
        return
    plan_path = fixture_root.joinpath(*plan_relative.parts)
    result_path = fixture_root.joinpath(*result_relative.parts)
    if file_sha256(plan_path) != pair["plan_file_sha256"]:
        _fail("pair_plan_hash_mismatch", "/plan_file_sha256", "plan raw digest mismatch")
    if file_sha256(result_path) != pair["result_file_sha256"]:
        _fail("pair_result_hash_mismatch", "/result_file_sha256", "result raw digest mismatch")
    plan = read_json_file(plan_path)
    result = read_json_file(result_path)
    validate_execution_plan(plan)
    validate_simulation_result(result)
    _require_plan_result_echo(plan, result)
    if result["status"] != pair["expected_status"]:
        _fail("pair_status_mismatch", "/expected_status", "expected status differs from result")
    if result["plan_file_sha256"] != pair["plan_file_sha256"]:
        _fail("result_plan_hash_mismatch", "/plan_file_sha256", "result does not echo raw plan digest")
    for index, check in enumerate(checks):
        try:
            plan_value = _resolve_pointer(plan, check["plan_json_pointer"])
            result_value = _resolve_pointer(result, check["result_json_pointer"])
        except KeyError as error:
            _fail("pair_pointer_missing", f"/equality_checks/{index}", f"missing JSON pointer: {error.args[0]}")
        if plan_value != result_value:
            _fail("pair_value_mismatch", f"/equality_checks/{index}", "pointed values differ")


def validate_capabilities_projection(document: dict[str, Any]) -> None:
    _schema(document, CAPABILITIES_PROJECTION_SCHEMA_ID)
    expected_arrays = {
        "transport_modes": list(TRANSPORT_MODES),
        "execution_modes": list(EXECUTION_MODES),
        "dependency_modes": list(DEPENDENCY_MODES),
        "path_preparation_policies": list(PATH_PREPARATION_POLICIES),
    }
    for field, expected in expected_arrays.items():
        if document[field] != expected:
            _fail("capability_enum_mismatch", f"/{field}", f"expected {expected}")
    identities = {
        "plan_schema": PLAN_SCHEMA_ID,
        "result_schema": RESULT_SCHEMA_ID,
        "operation_event_schema": OPERATION_EVENT_SCHEMA_ID,
    }
    for field, schema_id in identities.items():
        if document[field]["schema_id"] != schema_id:
            _fail("capability_schema_id_mismatch", f"/{field}/schema_id", f"expected {schema_id}")
        if document[field]["raw_sha256"] != file_sha256(SCHEMA_FILES[schema_id]):
            _fail("capability_schema_hash_mismatch", f"/{field}/raw_sha256", "schema raw digest mismatch")
    limits = read_json_file(ABI_LIMITS_PATH)
    if set(limits) != {
        "max_plan_file_bytes",
        "max_result_file_bytes",
        "min_mtu_bytes",
        "max_mtu_bytes",
        "max_payload_bytes",
        "max_pipe_inflight_transit_units",
    }:
        _fail("abi_limit_keys", "/abi_limits", "ABI limit file must have exactly six fields")
    expected_limits = dict(limits)
    expected_limits["abi_limits_raw_sha256"] = file_sha256(ABI_LIMITS_PATH)
    if document["abi_limits"] != expected_limits:
        _fail("capability_abi_limits_mismatch", "/abi_limits", "ABI limits or raw digest mismatch")
    if limits["max_payload_bytes"] != UINT64_MAX or limits["max_pipe_inflight_transit_units"] != 1073741823:
        _fail("abi_limit_value", "/abi_limits", "fixed payload/Pipe limits differ from contract")


def validate_operation_event(event: dict[str, Any]) -> None:
    _schema(event, OPERATION_EVENT_SCHEMA_ID)
    event_type = event["event_type"]
    entity_fields = {
        "plane_id",
        "program_epoch_id",
        "physical_config_generation",
        "configuration_id",
        "step_id",
        "flow_group_id",
        "flow_id",
        "token_id",
    }
    required: set[str]
    if event_type == "token_ready":
        required = {"token_id"}
    elif event_type in {"epoch_open", "path_prep_start", "path_ready", "epoch_close", "reconfiguration_start", "reconfiguration_end"}:
        required = {"plane_id", "program_epoch_id", "configuration_id"}
    elif event_type in {"group_release", "group_complete"}:
        required = {"plane_id", "program_epoch_id", "configuration_id", "step_id", "flow_group_id"}
    elif event_type in {"flow_last_sent", "flow_complete"}:
        required = {"plane_id", "program_epoch_id", "configuration_id", "step_id", "flow_group_id", "flow_id"}
    elif event_type in {"step_complete", "lockstep_all_paths_ready"}:
        required = {"step_id"}
    elif event_type == "simulation_complete":
        required = set()
    else:
        required = {field for field in entity_fields if event[field] is not None}
    for field in required:
        if event[field] is None:
            _fail("operation_event_nullability", f"/{field}", f"{event_type} requires {field}")
    if event_type != "blocked_guard_snapshot":
        unexpected = entity_fields - required
        for field in unexpected:
            if field == "physical_config_generation" and event_type in {"epoch_open", "path_ready", "epoch_close", "reconfiguration_start", "reconfiguration_end", "group_release", "group_complete", "flow_last_sent", "flow_complete"}:
                continue
            if event[field] is not None:
                _fail("operation_event_extra_entity", f"/{field}", f"{event_type} must not carry {field}")
        if event["reason_code"] is not None:
            _fail("operation_event_reason", "/reason_code", "only blocked snapshot has a reason")
    elif event["reason_code"] is None:
        _fail("operation_event_reason", "/reason_code", "blocked snapshot requires a reason")
