#ifndef OVERLAP4OCS_HTSIM_OCS_RECONFIGURATION_H
#define OVERLAP4OCS_HTSIM_OCS_RECONFIGURATION_H

#include <cstdint>
#include <functional>
#include <optional>

#include "eventlist.h"

namespace htsim_ocs {

class OcsReconfigurationTimer final : public EventSource {
  public:
    using CompletionCallback =
        std::function<void(std::uint64_t, std::uint64_t)>;

    OcsReconfigurationTimer(EventList& event_list, std::uint64_t plane_id,
                            CompletionCallback callback);

    void start(std::uint64_t program_epoch_id, std::uint64_t end_ps);
    void cancel() noexcept;
    void doNextEvent() override;
    bool pending() const noexcept { return program_epoch_id_.has_value(); }

  private:
    std::uint64_t plane_id_;
    CompletionCallback callback_;
    std::optional<std::uint64_t> program_epoch_id_;
    std::optional<std::uint64_t> end_ps_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_RECONFIGURATION_H
