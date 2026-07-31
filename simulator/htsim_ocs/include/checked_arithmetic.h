#ifndef OVERLAP4OCS_HTSIM_OCS_CHECKED_ARITHMETIC_H
#define OVERLAP4OCS_HTSIM_OCS_CHECKED_ARITHMETIC_H

#include <cstdint>
#include <limits>

namespace htsim_ocs {

__extension__ typedef unsigned __int128 uint128_t;

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

inline bool u128_to_u64(uint128_t value, std::uint64_t& result) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    result = static_cast<std::uint64_t>(value);
    return true;
}

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_CHECKED_ARITHMETIC_H
