#pragma once
#include <array>
#include <bit>
#include <cstdint>

namespace hydra::dsp {
// Five signed 32x32 products can overflow int64_t. Keep their exact sum as a
// two's-complement pair, then arithmetic-shift and saturate. No compiler-specific
// 128-bit type is required (including in the MSVC interpreter fallback).
inline std::uint32_t biquadResult(const std::array<std::int64_t, 5>& products, unsigned shift) {
    std::uint64_t low = 0;
    std::int64_t high = 0;
    for(const auto product : products) {
        const auto next = low + static_cast<std::uint64_t>(product);
        high += (product < 0 ? -1 : 0) + (next < low ? 1 : 0);
        low = next;
    }
    if(shift >= 128) return high < 0 ? 0xFFFFFFFFu : 0;
    if(shift >= 64) {
        low = std::bit_cast<std::uint64_t>(high >> (shift - 64));
        high = high < 0 ? -1 : 0;
    } else if(shift) {
        low = (low >> shift) | (static_cast<std::uint64_t>(high) << (64 - shift));
        high >>= shift;
    }
    if(high == 0 && low <= 0x7FFFFFFFu) return static_cast<std::uint32_t>(low);
    if(high == -1 && low >= 0xFFFFFFFF80000000ull) return static_cast<std::uint32_t>(low);
    return high < 0 ? 0x80000000u : 0x7FFFFFFFu;
}
}
