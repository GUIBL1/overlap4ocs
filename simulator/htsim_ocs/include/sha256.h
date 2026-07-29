#ifndef OVERLAP4OCS_HTSIM_OCS_SHA256_H
#define OVERLAP4OCS_HTSIM_OCS_SHA256_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace htsim_ocs {

class Sha256 final {
  public:
    Sha256();

    void update(const void* data, std::size_t size);
    std::array<std::uint8_t, 32> finalize();

  private:
    void transform(const std::uint8_t block[64]);

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_size_ = 0;
    bool finalized_ = false;
};

std::string sha256_hex(std::string_view bytes);

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_SHA256_H
