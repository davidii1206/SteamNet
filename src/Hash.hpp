#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace steamnet::detail {

inline constexpr std::uint64_t FnvOffset = 14695981039346656037ull;
inline constexpr std::uint64_t FnvPrime = 1099511628211ull;

inline std::uint64_t FnvAppend(std::uint64_t hash, std::span<const std::uint8_t> bytes) noexcept
{
    for (const std::uint8_t byte : bytes)
    {
        hash = (hash ^ byte) * FnvPrime;
    }
    return hash;
}

inline std::uint64_t FnvAppend(std::uint64_t hash, std::string_view text) noexcept
{
    for (const char c : text)
    {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * FnvPrime;
    }
    // Separator so that ("ab","c") and ("a","bc") hash differently.
    return (hash ^ 0xFFu) * FnvPrime;
}

template<typename T>
    requires std::is_integral_v<T>
std::uint64_t FnvAppend(std::uint64_t hash, T value) noexcept
{
    auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i)
    {
        hash = (hash ^ ((bits >> (8 * i)) & 0xFFu)) * FnvPrime;
    }
    return hash;
}

} // namespace steamnet::detail
