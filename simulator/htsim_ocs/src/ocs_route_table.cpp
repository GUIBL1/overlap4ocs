#include "ocs_route_table.h"

#include <stdexcept>

#include "ocs_sink.h"
#include "ocs_switch.h"
#include "pipe.h"

namespace htsim_ocs {

OcsRouteTable::OcsRouteTable(OcsSwitch& ocs_switch,
                             const std::vector<Pipe*>& pipes,
                             const std::vector<OcsSink*>& sinks)
    : switch_(&ocs_switch), pipes_(pipes), sinks_(sinks) {
    if (pipes_.size() != sinks_.size()) {
        throw std::invalid_argument("pipe and sink route-table sizes differ");
    }
    routes_.reserve(pipes_.size());
    for (std::size_t dst = 0; dst < pipes_.size(); ++dst) {
        if (pipes_[dst] == nullptr || sinks_[dst] == nullptr) {
            throw std::invalid_argument("route table contains a null hop");
        }
        auto route = std::make_unique<Route>(3);
        route->push_back(switch_);
        route->push_back(pipes_[dst]);
        route->push_back(sinks_[dst]);
        routes_.push_back(std::move(route));
    }
}

const Route& OcsRouteTable::route_for_destination(
    std::uint64_t dst_rank) const {
    if (dst_rank >= routes_.size()) {
        throw std::out_of_range("destination rank is outside route table");
    }
    return *routes_[static_cast<std::size_t>(dst_rank)];
}

bool OcsRouteTable::has_exact_three_hop_routes() const noexcept {
    for (std::size_t dst = 0; dst < routes_.size(); ++dst) {
        const Route& route = *routes_[dst];
        if (route.size() != 3 || route.at(0) != switch_ ||
            route.at(1) != pipes_[dst] || route.at(2) != sinks_[dst]) {
            return false;
        }
    }
    return true;
}

}  // namespace htsim_ocs
