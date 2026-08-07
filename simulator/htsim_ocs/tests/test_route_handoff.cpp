#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "eventlist.h"
#include "ocs_dataplane_error.h"
#include "ocs_packet.h"
#include "ocs_packet_flow_bridge.h"
#include "ocs_switch.h"
#include "route.h"

namespace {

class FreeingSink final : public PacketSink {
  public:
    void receivePacket(Packet& packet) override {
        ++received_;
        packet.free();
    }
    const std::string& nodename() override { return name_; }
    std::uint64_t received() const noexcept { return received_; }

  private:
    std::string name_ = "freeing-sink";
    std::uint64_t received_ = 0;
};

class AdvanceSource final : public EventSource {
  public:
    explicit AdvanceSource(EventList& event_list)
        : EventSource(event_list, "advance-source") {}
    void doNextEvent() override {}
};

}  // namespace

int main(int argc, char** argv) {
    using namespace htsim_ocs;
    if (argc != 2) {
        std::cerr << "usage: test_route_handoff CASE\n";
        return 2;
    }
    const std::string test_case = argv[1];
    try {
        EventList event_list;
        AdvanceSource advance_source(event_list);
        std::uint64_t entered = 0;
        const std::uint64_t latency =
            test_case == "time-overflow" ? 1 : 20;
        OcsSwitch ocs_switch(0, latency, [&entered]() { ++entered; },
                             [&entered]() { --entered; });
        ocs_switch.install(7, 5, 3, {1, 0});
        FreeingSink sink;
        Route route(2);
        route.push_back(&ocs_switch);
        route.push_back(&sink);
        OcsPacketFlowBridge bridge;
        OcsPacketPool<OcsPacket> pool;
        OcsPacketMetadata metadata{99, 88, 77, 0, 7, 5, 3, 0, 1, 0, 0};
        std::string expected_error;
        if (test_case == "wrong-dst") {
            metadata.dst_rank = 0;
            expected_error = "route_mismatch";
        } else if (test_case == "wrong-config") {
            metadata.configuration_id = 4;
            expected_error = "route_mismatch";
        } else if (test_case == "wrong-epoch") {
            metadata.program_epoch_id = 6;
            expected_error = "stale_program_epoch";
        } else if (test_case == "wrong-generation") {
            metadata.physical_config_generation = 2;
            expected_error = "stale_physical_generation";
        } else if (test_case == "wrong-plane") {
            metadata.plane_id = 1;
            expected_error = "packet_plane_mismatch";
        } else if (test_case == "time-overflow") {
            EventList::sourceIsPending(
                advance_source, std::numeric_limits<std::uint64_t>::max());
            if (!EventList::doNextEvent()) {
                throw std::runtime_error("failed to advance EventList time");
            }
            expected_error = "simulation_time_overflow";
        } else if (test_case != "accepted") {
            throw std::runtime_error("unknown route test case");
        }

        OcsPacket* packet = pool.allocate();
        packet->initialize(bridge.packet_flow(), route, metadata, 1, 0);
        bool rejected = false;
        try {
            packet->sendOn();
        } catch (const OcsDataplaneError& error) {
            rejected = error.error_code() == expected_error;
        }
        if (expected_error.empty()) {
            if (sink.received() != 1 || entered != 1) {
                throw std::runtime_error("accepted packet missed route handoff");
            }
        } else if (!rejected) {
            throw std::runtime_error("route guard returned the wrong error");
        }
        pool.verify_all_returned();
        std::cout << "route-handoff: PASS case=" << test_case << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "route-handoff: FAIL case=" << test_case
                  << " error=" << error.what() << '\n';
        return 1;
    }
}
