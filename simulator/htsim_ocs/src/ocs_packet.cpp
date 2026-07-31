#include "ocs_packet.h"

#include <limits>

namespace htsim_ocs {

void OcsPacket::pool_acquire(void* owner, std::uint64_t generation) {
    pool_ = static_cast<OcsPacketPool<OcsPacket>*>(owner);
    pool_owner_ = owner;
    pool_generation_ = generation;
    pool_active_ = true;
}

void OcsPacket::initialize(PacketFlow& flow, const Route& route,
                           const OcsPacketMetadata& metadata,
                           std::uint16_t payload_bytes,
                           std::uint64_t last_payload_sent_ps) {
    if (!pool_active_ || payload_bytes == 0) {
        throw OcsDataplaneError("packet_initialization_error",
                                "packet must be active with non-zero payload");
    }
    metadata_ = metadata;
    payload_bytes_ = payload_bytes;
    last_payload_sent_ps_ = last_payload_sent_ps;
    Packet::set_route(flow, route, static_cast<int>(payload_bytes), 0);
    set_dst(static_cast<std::uint32_t>(metadata.dst_rank));
}

void OcsPacket::reset_for_pool() {
    Packet::set_route(nullptr);
    _flow = nullptr;
    _dst = std::numeric_limits<std::uint32_t>::max();
    _pathid = std::numeric_limits<std::uint32_t>::max();
    _direction = NONE;
    _flags = 0;
    _size = 0;
    _oldsize = 0;
    metadata_ = {};
    payload_bytes_ = 0;
    last_payload_sent_ps_ = 0;
}

void OcsPacket::free() {
    if (pool_ == nullptr) {
        throw OcsDataplaneError("packet_pool_owner_mismatch",
                                "packet has no owning pool");
    }
    pool_->release(this, pool_generation_);
}

void OcsTransitBatch::pool_acquire(void* owner, std::uint64_t generation) {
    pool_ = static_cast<OcsPacketPool<OcsTransitBatch>*>(owner);
    pool_owner_ = owner;
    pool_generation_ = generation;
    pool_active_ = true;
}

void OcsTransitBatch::initialize(PacketFlow& flow, const Route& route,
                                 const OcsPacketMetadata& metadata,
                                 std::uint64_t logical_payload_bytes,
                                 std::uint64_t logical_packet_count,
                                 std::uint16_t tail_payload_bytes,
                                 std::uint64_t last_payload_sent_ps) {
    if (!pool_active_ || logical_payload_bytes == 0 ||
        logical_packet_count == 0 || tail_payload_bytes == 0) {
        throw OcsDataplaneError("packet_initialization_error",
                                "transit batch metadata must be non-zero");
    }
    metadata_ = metadata;
    logical_payload_bytes_ = logical_payload_bytes;
    logical_packet_count_ = logical_packet_count;
    tail_payload_bytes_ = tail_payload_bytes;
    last_payload_sent_ps_ = last_payload_sent_ps;
    Packet::set_route(flow, route, static_cast<int>(tail_payload_bytes), 0);
    set_dst(static_cast<std::uint32_t>(metadata.dst_rank));
}

void OcsTransitBatch::reset_for_pool() {
    Packet::set_route(nullptr);
    _flow = nullptr;
    _dst = std::numeric_limits<std::uint32_t>::max();
    _pathid = std::numeric_limits<std::uint32_t>::max();
    _direction = NONE;
    _flags = 0;
    _size = 0;
    _oldsize = 0;
    metadata_ = {};
    logical_payload_bytes_ = 0;
    logical_packet_count_ = 0;
    tail_payload_bytes_ = 0;
    last_payload_sent_ps_ = 0;
}

void OcsTransitBatch::free() {
    if (pool_ == nullptr) {
        throw OcsDataplaneError("packet_pool_owner_mismatch",
                                "transit batch has no owning pool");
    }
    pool_->release(this, pool_generation_);
}

}  // namespace htsim_ocs
