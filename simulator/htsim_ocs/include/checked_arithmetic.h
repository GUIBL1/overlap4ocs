#ifndef OVERLAP4OCS_HTSIM_OCS_CHECKED_ARITHMETIC_H
#define OVERLAP4OCS_HTSIM_OCS_CHECKED_ARITHMETIC_H

#include <cstdint>
#include <limits>

namespace htsim_ocs {

inline bool checked_add_u64(std::uint64_t left, std::uint64_t right,
                            std::uint64_t& result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

inline bool checked_mul_u64(std::uint64_t left, std::uint64_t right,
                            std::uint64_t& result) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_CHECKED_ARITHMETIC_H
