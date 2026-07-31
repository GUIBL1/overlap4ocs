#ifndef OVERLAP4OCS_HTSIM_OCS_PACKET_H
#define OVERLAP4OCS_HTSIM_OCS_PACKET_H

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "checked_arithmetic.h"
#include "network.h"
#include "ocs_dataplane_error.h"

namespace htsim_ocs {

template <typename P>
class OcsPacketPool;

struct OcsPacketMetadata {
    std::uint64_t flow_id = 0;
    std::uint64_t flow_group_id = 0;
    std::uint64_t step_id = 0;
    std::uint64_t plane_id = 0;
    std::uint64_t program_epoch_id = 0;
    std::uint64_t configuration_id = 0;
    std::uint64_t physical_config_generation = 0;
    std::uint64_t src_rank = 0;
    std::uint64_t dst_rank = 0;
    std::uint64_t byte_offset = 0;
    std::uint64_t logical_packet_ordinal = 0;
};

class OcsTransitUnitView {
  public:
    virtual ~OcsTransitUnitView() = default;
    virtual const OcsPacketMetadata& metadata() const noexcept = 0;
    virtual std::uint64_t first_offset() const noexcept = 0;
    virtual std::uint64_t logical_payload_bytes() const noexcept = 0;
    virtual std::uint64_t logical_packet_count() const noexcept = 0;
    virtual std::uint64_t tail_payload_bytes() const noexcept = 0;
    virtual std::uint64_t last_payload_sent_ps() const noexcept = 0;
    virtual bool pool_active() const noexcept = 0;
};

class OcsPacket final : public Packet, public OcsTransitUnitView {
  public:
    OcsPacket() = default;
    ~OcsPacket() override = default;

    void initialize(PacketFlow& flow, const Route& route,
                    const OcsPacketMetadata& metadata,
                    std::uint16_t payload_bytes,
                    std::uint64_t last_payload_sent_ps);
    PktPriority priority() const override { return Packet::PRIO_NONE; }
    void free() override;

    const OcsPacketMetadata& metadata() const noexcept override {
        return metadata_;
    }
    std::uint64_t first_offset() const noexcept override {
        return metadata_.byte_offset;
    }
    std::uint64_t logical_payload_bytes() const noexcept override {
        return payload_bytes_;
    }
    std::uint64_t logical_packet_count() const noexcept override { return 1; }
    std::uint64_t tail_payload_bytes() const noexcept override {
        return payload_bytes_;
    }
    std::uint64_t last_payload_sent_ps() const noexcept override {
        return last_payload_sent_ps_;
    }
    bool pool_active() const noexcept override { return pool_active_; }

  private:
    template <typename P>
    friend class OcsPacketPool;

    void pool_acquire(void* owner, std::uint64_t generation);
    void reset_for_pool();

    OcsPacketPool<OcsPacket>* pool_ = nullptr;
    void* pool_owner_ = nullptr;
    std::uint64_t pool_generation_ = 0;
    bool pool_active_ = false;
    OcsPacketMetadata metadata_{};
    std::uint64_t payload_bytes_ = 0;
    std::uint64_t last_payload_sent_ps_ = 0;
};

class OcsTransitBatch final : public Packet, public OcsTransitUnitView {
  public:
    OcsTransitBatch() = default;
    ~OcsTransitBatch() override = default;

    void initialize(PacketFlow& flow, const Route& route,
                    const OcsPacketMetadata& metadata,
                    std::uint64_t logical_payload_bytes,
                    std::uint64_t logical_packet_count,
                    std::uint16_t tail_payload_bytes,
                    std::uint64_t last_payload_sent_ps);
    PktPriority priority() const override { return Packet::PRIO_NONE; }
    void free() override;

    const OcsPacketMetadata& metadata() const noexcept override {
        return metadata_;
    }
    std::uint64_t first_offset() const noexcept override {
        return metadata_.byte_offset;
    }
    std::uint64_t logical_payload_bytes() const noexcept override {
        return logical_payload_bytes_;
    }
    std::uint64_t logical_packet_count() const noexcept override {
        return logical_packet_count_;
    }
    std::uint64_t tail_payload_bytes() const noexcept override {
        return tail_payload_bytes_;
    }
    std::uint64_t last_payload_sent_ps() const noexcept override {
        return last_payload_sent_ps_;
    }
    bool pool_active() const noexcept override { return pool_active_; }

  private:
    template <typename P>
    friend class OcsPacketPool;

    void pool_acquire(void* owner, std::uint64_t generation);
    void reset_for_pool();

    OcsPacketPool<OcsTransitBatch>* pool_ = nullptr;
    void* pool_owner_ = nullptr;
    std::uint64_t pool_generation_ = 0;
    bool pool_active_ = false;
    OcsPacketMetadata metadata_{};
    std::uint64_t logical_payload_bytes_ = 0;
    std::uint64_t logical_packet_count_ = 0;
    std::uint64_t tail_payload_bytes_ = 0;
    std::uint64_t last_payload_sent_ps_ = 0;
};

template <typename P>
class OcsPacketPool final {
  public:
    OcsPacketPool() = default;
    OcsPacketPool(const OcsPacketPool&) = delete;
    OcsPacketPool& operator=(const OcsPacketPool&) = delete;

    ~OcsPacketPool() { assert(in_use_count_ == 0); }

    P* allocate() {
        if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw OcsDataplaneError("packet_pool_generation_exhausted",
                                    "packet pool generation counter exhausted");
        }
        P* packet = nullptr;
        if (free_list_.empty()) {
            auto owned = std::make_unique<P>();
            packet = owned.get();
            objects_.push_back(std::move(owned));
            if (!checked_add_u64(allocated_count_, 1, allocated_count_)) {
                throw OcsDataplaneError("packet_pool_allocation_count_overflow",
                                        "packet pool allocation count overflow");
            }
        } else {
            packet = free_list_.back();
            free_list_.pop_back();
        }
        if (packet->pool_active_ || packet->ref_count() != 0 ||
            (packet->pool_owner_ != nullptr && packet->pool_owner_ != this)) {
            throw OcsDataplaneError("packet_pool_owner_mismatch",
                                    "packet pool object ownership is invalid");
        }
        packet->pool_acquire(this, next_generation_++);
        packet->inc_ref_count();
        if (!checked_add_u64(in_use_count_, 1, in_use_count_)) {
            throw OcsDataplaneError("packet_pool_in_use_count_overflow",
                                    "packet pool in-use count overflow");
        }
        if (in_use_count_ > peak_in_use_count_) {
            peak_in_use_count_ = in_use_count_;
        }
        return packet;
    }

    void release(P* packet, std::uint64_t expected_generation) {
        if (packet == nullptr || packet->pool_owner_ != this) {
            throw OcsDataplaneError("packet_pool_owner_mismatch",
                                    "packet returned to a non-owning pool");
        }
        if (!packet->pool_active_) {
            throw OcsDataplaneError("packet_pool_double_free",
                                    "packet was returned to its pool twice");
        }
        if (packet->pool_generation_ != expected_generation) {
            throw OcsDataplaneError("packet_pool_generation_mismatch",
                                    "packet pool generation does not match");
        }
        if (packet->ref_count() != 1 || in_use_count_ == 0) {
            throw OcsDataplaneError("packet_pool_refcount_mismatch",
                                    "packet pool refcount is invalid");
        }
        packet->reset_for_pool();
        packet->dec_ref_count();
        packet->pool_active_ = false;
        --in_use_count_;
        free_list_.push_back(packet);
    }

    void release(P* packet) {
        if (packet == nullptr) {
            throw OcsDataplaneError("packet_pool_owner_mismatch",
                                    "null packet returned to pool");
        }
        release(packet, packet->pool_generation_);
    }

    std::uint64_t allocated_count() const noexcept { return allocated_count_; }
    std::uint64_t in_use_count() const noexcept { return in_use_count_; }
    std::uint64_t peak_in_use_count() const noexcept { return peak_in_use_count_; }

    void verify_all_returned() const {
        if (in_use_count_ != 0) {
            throw OcsDataplaneError("packet_pool_leak",
                                    "packet pool still has live objects");
        }
    }

  private:
    std::vector<std::unique_ptr<P>> objects_;
    std::vector<P*> free_list_;
    std::uint64_t allocated_count_ = 0;
    std::uint64_t in_use_count_ = 0;
    std::uint64_t peak_in_use_count_ = 0;
    std::uint64_t next_generation_ = 1;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_PACKET_H
