#include "ocs_coordinator.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "checked_arithmetic.h"
#include "eventlist.h"
#include "ocs_dataplane_error.h"
#include "ocs_guards.h"
#include "ocs_watchdog.h"

namespace htsim_ocs {
namespace {

std::uint64_t group_payload_bytes(const OcsExecutionPlanV2& plan,
                                  const OcsFlowGroupSpec& group) {
    std::uint64_t total = 0;
    for (const std::uint64_t flow_id : group.flow_ids) {
        if (!checked_add_u64(total, plan.flow_by_id(flow_id).payload_bytes,
                             total)) {
            throw OcsDataplaneError("group_payload_overflow",
                                    "group payload total overflow");
        }
    }
    return total;
}

std::uint64_t generation_for_epoch(const OcsPlaneProgram& program,
                                   std::uint64_t program_epoch_id) {
    std::uint64_t generation = 0;
    for (const OcsProgramEpochSpec& epoch : program.epochs) {
        if (epoch.program_epoch_id > program_epoch_id) {
            break;
        }
        if (epoch.transition == "reconfigure" &&
            !checked_add_u64(generation, 1, generation)) {
            throw OcsDataplaneError(
                "physical_generation_overflow",
                "physical configuration generation overflow");
        }
    }
    return generation;
}

}  // namespace

OcsCoordinator::OcsCoordinator(
    EventList& event_list, std::shared_ptr<const OcsExecutionPlanV2> plan,
    OcsExecutionMode mode, OcsCapacityProof capacity_proof,
    OcsRuntimeFaultInjection fault_injection)
    : event_list_(event_list),
      plan_(std::move(plan)),
      mode_(mode),
      topology_(event_list_, plan_, mode_, std::move(capacity_proof),
                [this](const OcsFlowStats& stats) {
                    on_flow_complete(stats);
                },
                [this](const OcsFlowStats& stats) {
                    on_flow_last_sent(stats);
                }),
      dependencies_(*plan_),
      lockstep_gate_ready_(plan_->steps.size(), false),
      fault_injection_(std::move(fault_injection)) {
    groups_.reserve(plan_->flow_groups.size());
    for (const OcsFlowGroupSpec& group : plan_->flow_groups) {
        groups_.push_back(std::make_unique<OcsFlowGroupRuntime>(
            group,
            generation_for_epoch(plan_->plane_program_by_id(group.plane_id),
                                 group.program_epoch_id),
            group_payload_bytes(*plan_, group)));
    }
    steps_.reserve(plan_->steps.size());
    for (const OcsLogicalStepSpec& step : plan_->steps) {
        steps_.push_back(std::make_unique<OcsStepRuntime>(step));
    }
    planes_.reserve(plan_->plane_programs.size());
    for (const OcsPlaneProgram& program : plan_->plane_programs) {
        planes_.push_back(std::make_unique<OcsPlaneRuntime>(
            event_list_, program,
            [this](std::uint64_t plane_id, std::uint64_t epoch_id) {
                on_reconfiguration_complete(plane_id, epoch_id);
            }));
    }
}

void OcsCoordinator::initialize() {
    publish_token(0, 0);
    pump();
}

void OcsCoordinator::publish_token(std::uint64_t token_id,
                                   std::uint64_t time_ps) {
    const std::vector<std::uint64_t> newly_ready =
        dependencies_.publish(token_id, time_ps);
    OcsTraceContext context;
    context.token_id = token_id;
    trace_.record(time_ps, "token_ready", std::move(context));
    for (const std::uint64_t group_id : newly_ready) {
        groups_[static_cast<std::size_t>(group_id)]->mark_dependency_ready(
            time_ps);
    }
}

void OcsCoordinator::on_flow_last_sent(const OcsFlowStats& stats) {
    if (!stats.last_payload_sent_ps.has_value() ||
        *stats.last_payload_sent_ps != EventList::now()) {
        throw OcsDataplaneError("flow_send_sequence_mismatch",
                                "last-sent callback time is inconsistent");
    }
    const OcsFlowGroupSpec& group =
        plan_->flow_group_by_id(stats.flow_group_id);
    OcsTraceContext context;
    context.plane_id = group.plane_id;
    context.program_epoch_id = group.program_epoch_id;
    context.physical_config_generation =
        generation_for_epoch(plan_->plane_program_by_id(group.plane_id),
                             group.program_epoch_id);
    context.configuration_id = group.configuration_id;
    context.step_id = group.step_id;
    context.flow_group_id = group.flow_group_id;
    context.flow_id = stats.flow_id;
    trace_.record(EventList::now(), "flow_last_sent", std::move(context));
}

void OcsCoordinator::on_flow_complete(const OcsFlowStats& stats) {
    if (fault_injection_.suppress_flow_completion_id == stats.flow_id) {
        return;
    }
    process_flow_complete(stats);
    if (!duplicate_callback_injected_ &&
        fault_injection_.duplicate_flow_completion_id == stats.flow_id) {
        duplicate_callback_injected_ = true;
        process_flow_complete(stats);
    }
}

void OcsCoordinator::process_flow_complete(const OcsFlowStats& stats) {
    if (!stats.last_payload_received_ps.has_value() ||
        *stats.last_payload_received_ps != EventList::now()) {
        throw OcsDataplaneError("flow_receive_sequence_mismatch",
                                "flow completion callback time is inconsistent");
    }
    OcsFlowGroupRuntime& group =
        *groups_[static_cast<std::size_t>(stats.flow_group_id)];
    OcsTraceContext flow_context;
    flow_context.plane_id = group.spec().plane_id;
    flow_context.program_epoch_id = group.spec().program_epoch_id;
    flow_context.physical_config_generation = group.stats().physical_config_generation;
    flow_context.configuration_id = group.spec().configuration_id;
    flow_context.step_id = group.spec().step_id;
    flow_context.flow_group_id = group.spec().flow_group_id;
    flow_context.flow_id = stats.flow_id;
    trace_.record(EventList::now(), "flow_complete", std::move(flow_context));

    if (!group.note_flow_complete(stats.flow_id, EventList::now())) {
        return;
    }
    if (!checked_add_u64(completed_group_count_, 1,
                         completed_group_count_)) {
        throw OcsDataplaneError("group_completion_overflow",
                                "completed group counter overflow");
    }
    OcsTraceContext group_context;
    group_context.plane_id = group.spec().plane_id;
    group_context.program_epoch_id = group.spec().program_epoch_id;
    group_context.physical_config_generation = group.stats().physical_config_generation;
    group_context.configuration_id = group.spec().configuration_id;
    group_context.step_id = group.spec().step_id;
    group_context.flow_group_id = group.spec().flow_group_id;
    trace_.record(EventList::now(), "group_complete",
                  std::move(group_context));
    publish_token(group.spec().completion_token_id, EventList::now());

    OcsStepRuntime& step =
        *steps_[static_cast<std::size_t>(group.spec().step_id)];
    if (step.note_group_complete(group.spec().flow_group_id,
                                 EventList::now())) {
        OcsTraceContext step_context;
        step_context.step_id = step.spec().step_id;
        trace_.record(EventList::now(), "step_complete",
                      std::move(step_context));
        publish_token(step.spec().completion_token_id, EventList::now());
    }
}

void OcsCoordinator::on_reconfiguration_complete(
    std::uint64_t plane_id, std::uint64_t program_epoch_id) {
    OcsPlaneRuntime& plane =
        *planes_[static_cast<std::size_t>(plane_id)];
    plane.complete_reconfiguration(program_epoch_id, EventList::now());
    topology_.install_epoch_path(plane_id, program_epoch_id);
    const OcsProgramEpochRuntime& epoch = plane.current_epoch();

    OcsTraceContext end_context;
    end_context.plane_id = plane_id;
    end_context.program_epoch_id = program_epoch_id;
    end_context.physical_config_generation =
        epoch.physical_config_generation();
    end_context.configuration_id = epoch.spec().configuration_id;
    trace_.record(EventList::now(), "reconfiguration_end",
                  std::move(end_context));

    OcsTraceContext ready_context;
    ready_context.plane_id = plane_id;
    ready_context.program_epoch_id = program_epoch_id;
    ready_context.physical_config_generation =
        epoch.physical_config_generation();
    ready_context.configuration_id = epoch.spec().configuration_id;
    trace_.record(EventList::now(), "path_ready", ready_context);
    trace_.record(EventList::now(), "epoch_open", std::move(ready_context));
}

void OcsCoordinator::begin_current_epoch(OcsPlaneRuntime& plane) {
    OcsProgramEpochRuntime& epoch = plane.current_epoch();
    if (!topology_.plane_data_plane_idle(plane.plane_id())) {
        throw OcsDataplaneError("epoch_violation",
                                "path preparation started before plane drain");
    }
    const std::uint64_t time_ps = EventList::now();
    const bool immediate = plane.begin_path_preparation(
        time_ps, plan_->topology.reconfiguration_delay_ps);

    OcsTraceContext prep_context;
    prep_context.plane_id = plane.plane_id();
    prep_context.program_epoch_id = epoch.spec().program_epoch_id;
    prep_context.configuration_id = epoch.spec().configuration_id;
    if (epoch.spec().transition != "reconfigure") {
        prep_context.physical_config_generation =
            epoch.physical_config_generation();
    }
    trace_.record(time_ps, "path_prep_start", prep_context);

    if (epoch.spec().transition == "reconfigure") {
        OcsTraceContext reconfiguration_context = prep_context;
        if (epoch.physical_config_generation() == 0) {
            throw OcsDataplaneError("physical_generation_overflow",
                                    "reconfigure target generation is zero");
        }
        reconfiguration_context.physical_config_generation =
            epoch.physical_config_generation() - 1;
        trace_.record(time_ps, "reconfiguration_start",
                      std::move(reconfiguration_context));
        return;
    }
    if (!immediate) {
        throw OcsDataplaneError("epoch_violation",
                                "non-reconfigure transition was not immediate");
    }
    topology_.install_epoch_path(plane.plane_id(),
                                 epoch.spec().program_epoch_id);
    OcsTraceContext ready_context;
    ready_context.plane_id = plane.plane_id();
    ready_context.program_epoch_id = epoch.spec().program_epoch_id;
    ready_context.physical_config_generation =
        epoch.physical_config_generation();
    ready_context.configuration_id = epoch.spec().configuration_id;
    trace_.record(time_ps, "path_ready", ready_context);
    trace_.record(time_ps, "epoch_open", std::move(ready_context));
}

bool OcsCoordinator::close_completed_epochs() {
    bool progress = false;
    for (auto& plane_ptr : planes_) {
        OcsPlaneRuntime& plane = *plane_ptr;
        if (plane.program_complete()) {
            continue;
        }
        OcsProgramEpochRuntime& epoch = plane.current_epoch();
        if (!epoch.path_ready() ||
            !all_epoch_groups_complete(epoch.spec(), groups_)) {
            continue;
        }
        if (!topology_.plane_data_plane_idle(plane.plane_id())) {
            plane.mark_draining();
            continue;
        }
        std::uint64_t transfer_complete_ps = 0;
        for (const std::uint64_t group_id : epoch.spec().flow_group_ids) {
            const auto& completion =
                groups_[static_cast<std::size_t>(group_id)]->completion_ps();
            if (!completion.has_value()) {
                throw OcsDataplaneError("epoch_violation",
                                        "epoch group completion is missing");
            }
            transfer_complete_ps =
                std::max(transfer_complete_ps, *completion);
        }
        const OcsEpochRuntimeStats closed_identity = epoch.stats();
        plane.close_current(transfer_complete_ps, EventList::now());
        OcsTraceContext context;
        context.plane_id = plane.plane_id();
        context.program_epoch_id = closed_identity.program_epoch_id;
        context.physical_config_generation =
            closed_identity.physical_config_generation;
        context.configuration_id = closed_identity.configuration_id;
        trace_.record(EventList::now(), "epoch_close", std::move(context));
        progress = true;
    }
    return progress;
}

bool OcsCoordinator::prepare_permitted_paths() {
    bool progress = false;
    for (auto& plane_ptr : planes_) {
        OcsPlaneRuntime& plane = *plane_ptr;
        if (plane.program_complete()) {
            continue;
        }
        OcsProgramEpochRuntime& epoch = plane.current_epoch();
        if (epoch.path_preparation_started()) {
            continue;
        }
        if (!all_tokens_ready(dependencies_,
                              epoch.spec().path_prep_not_before_token_ids)) {
            continue;
        }
        begin_current_epoch(plane);
        progress = true;
    }
    return progress;
}

bool OcsCoordinator::update_lockstep_gates() {
    if (plan_->path_preparation_policy != "step_lockstep") {
        return false;
    }
    bool progress = false;
    for (const OcsLogicalStepSpec& step : plan_->steps) {
        const std::size_t index = static_cast<std::size_t>(step.step_id);
        if (!lockstep_gate_ready_[index] &&
            all_step_paths_ready(step, *plan_, planes_)) {
            lockstep_gate_ready_[index] = true;
            OcsTraceContext context;
            context.step_id = step.step_id;
            trace_.record(EventList::now(), "lockstep_all_paths_ready",
                          std::move(context));
            progress = true;
        }
    }
    return progress;
}

bool OcsCoordinator::release_permitted_groups() {
    bool progress = false;
    for (const OcsFlowGroupSpec& spec : plan_->flow_groups) {
        OcsFlowGroupRuntime& group =
            *groups_[static_cast<std::size_t>(spec.flow_group_id)];
        if (group.released() || !group.dependency_ready()) {
            continue;
        }
        OcsPlaneRuntime& plane =
            *planes_[static_cast<std::size_t>(spec.plane_id)];
        if (plane.program_complete() ||
            plane.current_epoch().spec().program_epoch_id !=
                spec.program_epoch_id ||
            !plane.current_epoch().path_ready()) {
            continue;
        }
        if (plan_->path_preparation_policy == "step_lockstep" &&
            !lockstep_gate_ready_[static_cast<std::size_t>(spec.step_id)]) {
            continue;
        }
        group.mark_released(EventList::now());
        steps_[static_cast<std::size_t>(spec.step_id)]->note_group_release(
            EventList::now());
        plane.note_transfer_start(EventList::now());
        topology_.release_group(spec.flow_group_id);

        OcsTraceContext context;
        context.plane_id = spec.plane_id;
        context.program_epoch_id = spec.program_epoch_id;
        context.physical_config_generation =
            plane.current_epoch().physical_config_generation();
        context.configuration_id = spec.configuration_id;
        context.step_id = spec.step_id;
        context.flow_group_id = spec.flow_group_id;
        trace_.record(EventList::now(), "group_release", std::move(context));
        progress = true;
    }
    return progress;
}

void OcsCoordinator::pump() {
    const std::uint64_t entity_count =
        static_cast<std::uint64_t>(plan_->readiness_tokens.size()) +
        static_cast<std::uint64_t>(plan_->steps.size()) +
        static_cast<std::uint64_t>(plan_->flow_groups.size()) +
        static_cast<std::uint64_t>(plan_->flows.size()) +
        static_cast<std::uint64_t>(plan_->plane_programs.size());
    const std::uint64_t max_rounds =
        entity_count > (std::numeric_limits<std::uint64_t>::max() - 16) / 8
            ? std::numeric_limits<std::uint64_t>::max()
            : entity_count * 8 + 16;
    for (std::uint64_t round = 0; round < max_rounds; ++round) {
        bool progress = false;
        progress = close_completed_epochs() || progress;
        progress = prepare_permitted_paths() || progress;
        progress = update_lockstep_gates() || progress;
        progress = release_permitted_groups() || progress;
        if (!progress) {
            return;
        }
    }
    throw OcsDataplaneError("coordinator_progress_overflow",
                            "coordinator fixed-point pump did not converge");
}

bool OcsCoordinator::run_complete() const {
    if (completed_group_count_ != plan_->flow_groups.size() ||
        !topology_.data_plane_idle()) {
        return false;
    }
    return std::all_of(planes_.begin(), planes_.end(),
                       [](const auto& plane) {
                           return plane->program_complete();
                       });
}

void OcsCoordinator::verify_success() const {
    const OcsStaticDataplaneAudit audit = topology_.audit();
    if (audit.expected_flow_count != plan_->traffic_inventory.expected_flow_count ||
        audit.completed_flow_count != audit.expected_flow_count ||
        completed_group_count_ !=
            plan_->traffic_inventory.expected_flow_group_count ||
        audit.expected_payload_bytes !=
            plan_->traffic_inventory.expected_payload_bytes ||
        audit.sent_payload_bytes != audit.expected_payload_bytes ||
        audit.received_payload_bytes != audit.expected_payload_bytes ||
        audit.in_flight_transit_unit_count != 0 || !audit.all_routes_exact ||
        !audit.all_pools_returned) {
        throw OcsDataplaneError("dataplane_audit_mismatch",
                                "runtime success audit did not balance");
    }
}

std::vector<OcsPlaneRuntimeStats> OcsCoordinator::plane_stats() const {
    std::vector<OcsPlaneRuntimeStats> result;
    result.reserve(planes_.size());
    for (const auto& plane_ptr : planes_) {
        const OcsPlaneRuntime& plane = *plane_ptr;
        const OcsPlaneDataplane& dataplane =
            topology_.plane_by_id(plane.plane_id());
        std::vector<std::uint64_t> pending_tokens;
        if (!plane.program_complete() &&
            !plane.current_epoch().path_preparation_started()) {
            for (const std::uint64_t token_id :
                 plane.current_epoch().spec().path_prep_not_before_token_ids) {
                if (!dependencies_.token_ready(token_id)) {
                    pending_tokens.push_back(token_id);
                }
            }
        }
        result.push_back(plane.stats(
            dataplane.serializer_backlog_flow_count(),
            dataplane.serializer_backlog_bytes(),
            dataplane.in_flight_transit_unit_count(),
            std::move(pending_tokens)));
    }
    return result;
}

OcsBlockedState OcsCoordinator::blocked_state(
    std::optional<std::uint64_t> next_event_time_ps) const {
    OcsBlockedState result;
    result.unfinished_token_ids = dependencies_.pending_token_ids();
    for (const auto& step : steps_) {
        if (!step->complete()) {
            result.unfinished_step_ids.push_back(step->spec().step_id);
        }
    }
    for (const auto& group : groups_) {
        if (!group->complete()) {
            result.unfinished_flow_group_ids.push_back(
                group->spec().flow_group_id);
        }
    }
    for (const OcsFlowStats& flow : topology_.all_flow_stats()) {
        if (!flow.complete) {
            result.unfinished_flow_ids.push_back(flow.flow_id);
        }
    }
    result.next_event_time_ps = next_event_time_ps;
    result.planes = plane_stats();
    return result;
}

OcsRunSummary OcsCoordinator::make_summary(
    std::string status, std::string stop_reason,
    std::optional<std::string> error_code,
    std::optional<std::uint64_t> next_event_time_ps) {
    const OcsStaticDataplaneAudit audit = topology_.audit();
    std::uint64_t completed_flows = 0;
    std::vector<OcsFlowStats> flows = topology_.all_flow_stats();
    for (const OcsFlowStats& flow : flows) {
        if (flow.complete) {
            ++completed_flows;
        }
    }
    OcsRuntimeTrafficSummary traffic{
        plan_->traffic_inventory.expected_flow_count,
        completed_flows,
        plan_->traffic_inventory.expected_flow_group_count,
        completed_group_count_,
        plan_->traffic_inventory.expected_payload_bytes,
        audit.sent_payload_bytes,
        audit.received_payload_bytes,
        audit.logical_packet_count,
        audit.simulated_transit_unit_count,
        audit.processed_event_count,
        audit.in_flight_transit_unit_count,
        audit.per_rank_sent_payload_bytes,
        audit.per_rank_received_payload_bytes,
        audit.per_plane_payload_bytes};

    std::vector<OcsStepRuntimeStats> step_stats;
    step_stats.reserve(steps_.size());
    for (const auto& step : steps_) {
        step_stats.push_back(step->stats());
    }
    std::vector<OcsFlowGroupRuntimeStats> group_stats;
    group_stats.reserve(groups_.size());
    for (const auto& group : groups_) {
        group_stats.push_back(group->stats());
    }
    std::vector<OcsPlaneRuntimeStats> planes = plane_stats();
    const bool success = status == "success";
    std::optional<OcsBlockedState> blocked;
    if (!success) {
        blocked = blocked_state(next_event_time_ps);
    }
    return OcsRunSummary{
        std::move(status),
        std::move(stop_reason),
        std::move(error_code),
        execution_mode_name(mode_),
        plan_->dependency_mode,
        plan_->path_preparation_policy,
        EventList::now(),
        success ? std::optional<std::uint64_t>(audit.collective_complete_ps)
                : std::nullopt,
        std::move(traffic),
        dependencies_.stats(),
        std::move(step_stats),
        std::move(group_stats),
        std::move(flows),
        std::move(planes),
        trace_.events(),
        std::move(blocked)};
}

void OcsCoordinator::cleanup_after_failure() {
    for (auto& plane : planes_) {
        plane->abort_timer();
    }
    topology_.abort_pending();
}

OcsRunSummary OcsCoordinator::run() {
    try {
        initialize();
        while (true) {
            if (run_complete()) {
                verify_success();
                trace_.record(EventList::now(), "simulation_complete");
                return make_summary("success", "collective_complete",
                                    std::nullopt, std::nullopt);
            }
            const OcsWatchdogDecision decision =
                OcsWatchdog::evaluate(plan_->run_limits);
            if (!decision.dispatch_next_event) {
                OcsTraceContext context;
                context.reason_code = decision.error_code;
                trace_.record(EventList::now(), "blocked_guard_snapshot",
                              std::move(context));
                OcsRunSummary summary = make_summary(
                    "failure", decision.stop_reason, decision.error_code,
                    decision.next_event_time_ps);
                cleanup_after_failure();
                return summary;
            }
            if (!EventList::doNextEvent()) {
                throw OcsDataplaneError("no_progress",
                                        "next event disappeared before dispatch");
            }
            pump();
        }
    } catch (const OcsDataplaneError& error) {
        std::uint64_t next_time = 0;
        const std::optional<std::uint64_t> next_event_time =
            EventList::nextEventTime(next_time)
                ? std::optional<std::uint64_t>(next_time)
                : std::nullopt;
        OcsTraceContext context;
        context.reason_code = error.error_code();
        trace_.record(EventList::now(), "blocked_guard_snapshot",
                      std::move(context));
        OcsRunSummary summary = make_summary(
            "failure", "invariant_violation", error.error_code(),
            next_event_time);
        cleanup_after_failure();
        return summary;
    } catch (const std::exception&) {
        std::uint64_t next_time = 0;
        const std::optional<std::uint64_t> next_event_time =
            EventList::nextEventTime(next_time)
                ? std::optional<std::uint64_t>(next_time)
                : std::nullopt;
        OcsTraceContext context;
        context.reason_code = "internal_error";
        trace_.record(EventList::now(), "blocked_guard_snapshot",
                      std::move(context));
        OcsRunSummary summary = make_summary(
            "failure", "internal_error", std::string("internal_error"),
            next_event_time);
        cleanup_after_failure();
        return summary;
    }
}

OcsRunSummary run_ocs_runtime(
    std::shared_ptr<const OcsExecutionPlanV2> plan,
    OcsRuntimeFaultInjection fault_injection) {
    const OcsExecutionMode mode = parse_execution_mode(plan->execution_mode);
    OcsCapacityProof proof = prove_capacity_before_event_sources(*plan, mode);
    EventList event_list;
    OcsCoordinator coordinator(event_list, std::move(plan), mode,
                               std::move(proof), std::move(fault_injection));
    return coordinator.run();
}

}  // namespace htsim_ocs
