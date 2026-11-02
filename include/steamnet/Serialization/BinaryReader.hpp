#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>

namespace steamnet {

// Bounds-checked counterpart of BinaryWriter. Reading past the end never touches memory outside the
// buffer: the reader enters a failed state, returns zero values from then on, and Ok() turns false.
// Network input is untrusted, so callers must check Ok() before acting on what they read.
class BinaryReader
{
public:
    BinaryReader() = default;
    explicit BinaryReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}
    BinaryReader(const void* data, std::size_t size) noexcept
        : data_(static_cast<const std::uint8_t*>(data), size)
    {
    }

    template<typename T>
        requires(std::is_arithmetic_v<T> && sizeof(T) <= 8)
    T Read()
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            return ReadBits<std::uint8_t>() != 0;
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            return std::bit_cast<T>(ReadBits<Bits>());
        }
        else
        {
            return static_cast<T>(ReadBits<std::make_unsigned_t<T>>());
        }
    }

    std::uint8_t ReadUInt8() { return Read<std::uint8_t>(); }
    std::uint16_t ReadUInt16() { return Read<std::uint16_t>(); }
    std::uint32_t ReadUInt32() { return Read<std::uint32_t>(); }
    std::uint64_t ReadUInt64() { return Read<std::uint64_t>(); }
    std::int32_t ReadInt32() { return Read<std::int32_t>(); }
    float ReadFloat() { return Read<float>(); }
    bool ReadBool() { return Read<bool>(); }

    // Strings longer than maxLength are rejected instead of allocated.
    std::string ReadString(std::size_t maxLength = 1u << 20)
    {
        const std::size_t length = ReadUInt32();
        if (length > maxLength || !Require(length))
        {
            failed_ = true;
            return {};
        }
        std::string result(reinterpret_cast<const char*>(data_.data() + position_), length);
        position_ += length;
        return result;
    }

    bool ReadBytes(void* out, std::size_t size)
    {
        if (!Require(size))
        {
            return false;
        }
        std::memcpy(out, data_.data() + position_, size);
        position_ += size;
        return true;
    }

    // Returns a reader over the next `size` bytes and advances past them.
    BinaryReader Slice(std::size_t size)
    {
        if (!Require(size))
        {
            return BinaryReader{};
        }
        BinaryReader slice(data_.subspan(position_, size));
        position_ += size;
        return slice;
    }

    bool Skip(std::size_t size)
    {
        if (!Require(size))
        {
            return false;
        }
        position_ += size;
        return true;
    }

    std::span<const std::uint8_t> Remainder() const noexcept { return data_.subspan(position_); }

    std::size_t Remaining() const noexcept { return failed_ ? 0 : data_.size() - position_; }
    bool AtEnd() const noexcept { return Remaining() == 0; }
    bool Ok() const noexcept { return !failed_; }
    void Fail() noexcept { failed_ = true; }

private:
    bool Require(std::size_t size)
    {
        if (failed_ || size > data_.size() - position_)
        {
            failed_ = true;
            return false;
        }
        return true;
    }

    template<typename Bits>
    Bits ReadBits()
    {
        if (!Require(sizeof(Bits)))
        {
            return 0;
        }
        Bits bits = 0;
        for (std::size_t i = 0; i < sizeof(Bits); ++i)
        {
            bits = static_cast<Bits>(bits | (static_cast<Bits>(data_[position_ + i]) << (8 * i)));
        }
        position_ += sizeof(Bits);
        return bits;
    }

    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

} // namespace steamnet
