#ifndef OVERLAP4OCS_HTSIM_OCS_DATAPLANE_ERROR_H
#define OVERLAP4OCS_HTSIM_OCS_DATAPLANE_ERROR_H

#include <stdexcept>
#include <string>
#include <utility>

#include "ocs_run_status.h"

namespace htsim_ocs {

class OcsDataplaneError final : public std::runtime_error {
  public:
    OcsDataplaneError(std::string error_code, std::string message,
                      ExitCode exit_code = ExitCode::kRuntimeInvariant)
        : std::runtime_error(std::move(message)),
          error_code_(std::move(error_code)),
          exit_code_(exit_code) {}

    const std::string& error_code() const noexcept { return error_code_; }
    ExitCode exit_code() const noexcept { return exit_code_; }

  private:
    std::string error_code_;
    ExitCode exit_code_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_DATAPLANE_ERROR_H
