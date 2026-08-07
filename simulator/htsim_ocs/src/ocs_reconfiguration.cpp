#include "ocs_reconfiguration.h"

#include <sstream>
#include <utility>

#include "ocs_dataplane_error.h"

namespace htsim_ocs {

namespace {

std::string timer_name(std::uint64_t plane_id) {
    std::ostringstream stream;
    stream << "ocs-reconfiguration-plane-" << plane_id;
    return stream.str();
}

}  // namespace

OcsReconfigurationTimer::OcsReconfigurationTimer(
    EventList& event_list, std::uint64_t plane_id,
    CompletionCallback callback)
    : EventSource(event_list, timer_name(plane_id)),
      plane_id_(plane_id),
      callback_(std::move(callback)) {}

void OcsReconfigurationTimer::start(std::uint64_t program_epoch_id,
                                    std::uint64_t end_ps) {
    if (pending() || end_ps < EventList::now()) {
        throw OcsDataplaneError("epoch_violation",
                                "reconfiguration timer start is invalid");
    }
    program_epoch_id_ = program_epoch_id;
    end_ps_ = end_ps;
    EventList::sourceIsPending(*this, end_ps);
}

void OcsReconfigurationTimer::cancel() noexcept {
    if (pending()) {
        EventList::cancelPendingSource(*this);
        program_epoch_id_.reset();
        end_ps_.reset();
    }
}

void OcsReconfigurationTimer::doNextEvent() {
    if (!program_epoch_id_.has_value() || !end_ps_.has_value() ||
        EventList::now() != *end_ps_) {
        throw OcsDataplaneError("epoch_violation",
                                "reconfiguration timer fired unexpectedly");
    }
    const std::uint64_t epoch_id = *program_epoch_id_;
    program_epoch_id_.reset();
    end_ps_.reset();
    if (callback_) {
        callback_(plane_id_, epoch_id);
    }
}

}  // namespace htsim_ocs
