#ifndef OVERLAP4OCS_HTSIM_OCS_VALIDATION_ERROR_H
#define OVERLAP4OCS_HTSIM_OCS_VALIDATION_ERROR_H

#include <stdexcept>
#include <string>
#include <utility>

#include "ocs_run_status.h"

namespace htsim_ocs {

class OcsValidationError final : public std::runtime_error {
  public:
    OcsValidationError(std::string error_code, std::string json_pointer,
                       std::string message,
                       ExitCode exit_code = ExitCode::kInvalidOrUnsupported)
        : std::runtime_error(std::move(message)),
          error_code_(std::move(error_code)),
          json_pointer_(std::move(json_pointer)),
          exit_code_(exit_code) {}

    const std::string& error_code() const noexcept { return error_code_; }
    const std::string& json_pointer() const noexcept { return json_pointer_; }
    ExitCode exit_code() const noexcept { return exit_code_; }

  private:
    std::string error_code_;
    std::string json_pointer_;
    ExitCode exit_code_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_VALIDATION_ERROR_H
