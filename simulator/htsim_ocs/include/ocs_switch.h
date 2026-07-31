#ifndef OVERLAP4OCS_HTSIM_OCS_SWITCH_H
#define OVERLAP4OCS_HTSIM_OCS_SWITCH_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "network.h"

namespace htsim_ocs {

class OcsSwitch final : public PacketSink {
  public:
    using TransitEnterCallback = std::function<void()>;
    using TransitRollbackCallback = std::function<void()>;

    OcsSwitch(std::uint64_t plane_id, std::uint64_t data_latency_ps,
              TransitEnterCallback transit_enter_callback,
              TransitRollbackCallback transit_rollback_callback);

    void install(std::uint64_t program_epoch_id,
                 std::uint64_t configuration_id,
                 std::uint64_t physical_config_generation,
                 std::vector<std::uint64_t> permutation);
    void receivePacket(Packet& packet) override;
    const std::string& nodename() override { return name_; }

    std::uint64_t plane_id() const noexcept { return plane_id_; }
    bool installed() const noexcept { return installed_; }
    std::uint64_t program_epoch_id() const noexcept {
        return program_epoch_id_;
    }
    std::uint64_t configuration_id() const noexcept { return configuration_id_; }
    std::uint64_t physical_config_generation() const noexcept {
        return physical_config_generation_;
    }

  private:
    [[noreturn]] void reject(Packet& packet, const char* error_code,
                             const char* message) const;

    std::uint64_t plane_id_;
    std::uint64_t data_latency_ps_;
    TransitEnterCallback transit_enter_callback_;
    TransitRollbackCallback transit_rollback_callback_;
    std::string name_;
    bool installed_ = false;
    std::uint64_t program_epoch_id_ = 0;
    std::uint64_t configuration_id_ = 0;
    std::uint64_t physical_config_generation_ = 0;
    std::vector<std::uint64_t> permutation_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_SWITCH_H
