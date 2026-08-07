#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ocs_dataplane_error.h"
#include "ocs_flow.h"
#include "ocs_packet.h"
#include "ocs_packet_flow_bridge.h"
#include "route.h"

namespace {

class NullSink final : public PacketSink {
  public:
    void receivePacket(Packet&) override {}
    const std::string& nodename() override { return name_; }

  private:
    std::string name_ = "null-sink";
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    using namespace htsim_ocs;
    try {
        OcsPacketFlowBridge bridge;
        require(!bridge.packet_flow().log_me(),
                "PacketFlow bridge unexpectedly enables logging");

        NullSink sink;
        Route route(1);
        route.push_back(&sink);
        OcsPacketMetadata metadata{UINT64_MAX,
                                   UINT64_MAX - 1,
                                   UINT64_MAX - 2,
                                   0,
                                   UINT64_MAX - 3,
                                   UINT64_MAX - 4,
                                   UINT64_MAX - 5,
                                   0,
                                   1,
                                   UINT64_MAX - 6,
                                   UINT64_MAX - 7};

        OcsPacketPool<OcsPacket> packet_pool;
        OcsPacket* packet = packet_pool.allocate();
        require(packet->priority() == Packet::PRIO_NONE,
                "OCS packet priority differs from PRIO_NONE");
        packet->initialize(bridge.packet_flow(), route, metadata, 17, 123);
        require(packet->size() == 17 && packet->metadata().flow_id == UINT64_MAX,
                "packet metadata was truncated");

        bool wrong_generation = false;
        try {
            packet_pool.release(packet, 2);
        } catch (const OcsDataplaneError& error) {
            wrong_generation = error.error_code() ==
                               "packet_pool_generation_mismatch";
        }
        require(wrong_generation, "wrong packet generation was accepted");
        packet->free();

        bool double_free = false;
        try {
            packet->free();
        } catch (const OcsDataplaneError& error) {
            double_free = error.error_code() == "packet_pool_double_free";
        }
        require(double_free, "packet double-free was accepted");

        OcsPacket* reused = packet_pool.allocate();
        require(reused == packet && packet_pool.allocated_count() == 1,
                "packet pool did not reuse its object");
        OcsPacketPool<OcsPacket> other_pool;
        bool wrong_owner = false;
        try {
            other_pool.release(reused);
        } catch (const OcsDataplaneError& error) {
            wrong_owner = error.error_code() == "packet_pool_owner_mismatch";
        }
        require(wrong_owner, "non-owning packet pool accepted an object");
        reused->free();
        packet_pool.verify_all_returned();

        OcsPacketPool<OcsTransitBatch> batch_pool;
        OcsTransitBatch* batch = batch_pool.allocate();
        constexpr std::uint64_t payload = 5ULL * 1024 * 1024 * 1024;
        constexpr std::uint64_t packets = (payload + 1499) / 1500;
        constexpr std::uint16_t tail = payload % 1500;
        batch->initialize(bridge.packet_flow(), route, metadata, payload, packets,
                          tail, UINT64_MAX);
        require(batch->priority() == Packet::PRIO_NONE &&
                    batch->logical_payload_bytes() == payload &&
                    batch->logical_packet_count() == packets &&
                    batch->size() == tail,
                "5 GiB transit batch metadata was truncated");
        batch->free();
        batch_pool.verify_all_returned();

        OcsFlowSpec flow_spec{0, 0, 0, 1, 10, {}};
        std::uint64_t completion_callbacks = 0;
        OcsFlow flow(flow_spec, OcsFlowRuntimeIdentity{0, 0, 0, 0, 0},
                     [&completion_callbacks](const OcsFlowStats&) {
                         ++completion_callbacks;
                     });
        flow.mark_released(0);
        flow.note_sent(0, 10, 1, 10, 1);
        bool gap_rejected = false;
        try {
            flow.note_received(1, 9, 1, 2);
        } catch (const OcsDataplaneError& error) {
            gap_rejected = error.error_code() ==
                           "flow_receive_sequence_mismatch";
        }
        require(gap_rejected, "receive gap was accepted");
        flow.note_received(0, 10, 1, 2);
        require(flow.complete() && completion_callbacks == 1,
                "flow completion callback count is wrong");
        bool duplicate_rejected = false;
        try {
            flow.note_received(0, 10, 1, 2);
        } catch (const OcsDataplaneError& error) {
            duplicate_rejected = error.error_code() ==
                                 "flow_receive_sequence_mismatch";
        }
        require(duplicate_rejected && completion_callbacks == 1,
                "duplicate flow completion was accepted");

        std::cout << "packet-adapter: PASS allocated="
                  << packet_pool.allocated_count() << " batch_payload="
                  << payload << " logical_packets=" << packets << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "packet-adapter: FAIL: " << error.what() << '\n';
        return 1;
    }
}
