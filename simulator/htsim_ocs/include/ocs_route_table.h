#ifndef OVERLAP4OCS_HTSIM_OCS_ROUTE_TABLE_H
#define OVERLAP4OCS_HTSIM_OCS_ROUTE_TABLE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "route.h"

class Pipe;

namespace htsim_ocs {

class OcsSink;
class OcsSwitch;

class OcsRouteTable final {
  public:
    OcsRouteTable(OcsSwitch& ocs_switch, const std::vector<Pipe*>& pipes,
                  const std::vector<OcsSink*>& sinks);

    const Route& route_for_destination(std::uint64_t dst_rank) const;
    std::size_t size() const noexcept { return routes_.size(); }
    bool has_exact_three_hop_routes() const noexcept;

  private:
    OcsSwitch* switch_;
    std::vector<Pipe*> pipes_;
    std::vector<OcsSink*> sinks_;
    std::vector<std::unique_ptr<Route>> routes_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_ROUTE_TABLE_H
