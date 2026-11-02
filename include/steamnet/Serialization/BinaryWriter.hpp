#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace steamnet {

// Little-endian binary archive. Every value has an explicit width, so the wire format does not depend on
// the compiler, struct padding or platform.
class BinaryWriter
{
public:
    template<typename T>
        requires(std::is_arithmetic_v<T> && sizeof(T) <= 8)
    void Write(T value)
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            buffer_.push_back(value ? 1 : 0);
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            WriteBits(std::bit_cast<Bits>(value));
        }
        else
        {
            using Bits = std::make_unsigned_t<T>;
            WriteBits(static_cast<Bits>(value));
        }
    }

    void Write(std::string_view value)
    {
        Write(static_cast<std::uint32_t>(value.size()));
        WriteBytes(value.data(), value.size());
    }

    void Write(const std::string& value) { Write(std::string_view(value)); }
    void Write(const char* value) { Write(std::string_view(value)); }

    void WriteBytes(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + size);
    }

    void WriteBytes(std::span<const std::uint8_t> bytes) { WriteBytes(bytes.data(), bytes.size()); }

    // Reserves a 32-bit length prefix and returns its position; call PatchLength once the body is written.
    std::size_t BeginLengthPrefixed()
    {
        const std::size_t position = buffer_.size();
        Write(std::uint32_t{0});
        return position;
    }

    void EndLengthPrefixed(std::size_t position)
    {
        const auto length = static_cast<std::uint32_t>(buffer_.size() - position - sizeof(std::uint32_t));
        for (std::size_t i = 0; i < 4; ++i)
        {
            buffer_[position + i] = static_cast<std::uint8_t>((length >> (8 * i)) & 0xFFu);
        }
    }

    std::size_t Size() const noexcept { return buffer_.size(); }
    bool Empty() const noexcept { return buffer_.empty(); }
    void Clear() noexcept { buffer_.clear(); }
    void Truncate(std::size_t size) { buffer_.resize(size); }

    std::span<const std::uint8_t> Bytes() const noexcept { return buffer_; }
    const std::vector<std::uint8_t>& Buffer() const noexcept { return buffer_; }
    std::vector<std::uint8_t> Release() noexcept { return std::move(buffer_); }

private:
    template<typename Bits>
    void WriteBits(Bits bits)
    {
        for (std::size_t i = 0; i < sizeof(Bits); ++i)
        {
            buffer_.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFFu));
        }
    }

    std::vector<std::uint8_t> buffer_;
};

} // namespace steamnet
