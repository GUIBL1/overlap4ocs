#include <cstdint>
#include <iostream>
#include <string>

#include "eventlist.h"
#include "network.h"
#include "pipe.h"
#include "route.h"

static_assert(sizeof(int) == 4,
              "HTSim Packet size ABI requires a 32-bit int");

namespace {

constexpr simtime_picosec kPipeDelayPs = 100;
constexpr int kPacketSizeBytes = 64;

class ProbePacket final : public Packet {
  public:
    explicit ProbePacket(int& free_count) : free_count_(free_count) {}

    PktPriority priority() const override { return PRIO_NONE; }
    void free() override { ++free_count_; }

  private:
    int& free_count_;
};

class ProbeSink final : public PacketSink {
  public:
    void receivePacket(Packet& packet) override {
        ++receive_count;
        received_bytes += packet.size();
        packet.free();
    }

    const std::string& nodename() override { return name_; }

    int receive_count = 0;
    std::uint64_t received_bytes = 0;

  private:
    std::string name_ = "probe_sink";
};

class ProbeKickoff final : public EventSource {
  public:
    ProbeKickoff(EventList& event_list, Packet& packet)
        : EventSource(event_list, "probe_kickoff"), packet_(packet) {
        event_list.sourceIsPending(*this, 0);
    }

    void doNextEvent() override { packet_.sendOn(); }

  private:
    Packet& packet_;
};

}  // namespace

int main() {
    EventList event_list;
    EventList::setEndtime(kPipeDelayPs + 1);

    PacketFlow flow(nullptr);
    Pipe pipe(kPipeDelayPs, event_list);
    ProbeSink sink;
    Route route;
    route.push_back(&pipe);
    route.push_back(&sink);

    int free_count = 0;
    ProbePacket packet(free_count);
    packet.set_route(flow, route, kPacketSizeBytes, 1);
    ProbeKickoff kickoff(event_list, packet);

    while (EventList::doNextEvent()) {
    }

    const bool passed = sink.receive_count == 1 && free_count == 1 &&
                        sink.received_bytes == kPacketSizeBytes &&
                        EventList::now() == kPipeDelayPs;
    if (!passed) {
        std::cerr << "core-link-probe failed: received=" << sink.receive_count
                  << " freed=" << free_count
                  << " bytes=" << sink.received_bytes
                  << " now_ps=" << EventList::now() << '\n';
        return 1;
    }

    std::cout << "core-link-probe: PASS (received=1 freed=1 bytes=64 "
                 "arrival_ps=100)\n";
    return 0;
}
