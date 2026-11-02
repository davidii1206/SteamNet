#pragma once

#include "steamnet/Serialization/BinaryReader.hpp"
#include "steamnet/Serialization/BinaryWriter.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace steamnet {

// Serializer<T> turns a value into bytes and back. The primary template handles:
//   - arithmetic types, bool and enums
//   - std::string, std::vector, std::array, std::optional, std::pair, std::map, std::unordered_map
//   - plain aggregate structs (up to 16 fields, no C arrays), by serializing the fields in order
// Everything else (or anything needing a custom format, e.g. quantized or compressed data) is handled by
// specializing Serializer<T> with static Write(BinaryWriter&, const T&) and static T Read(BinaryReader&).
template<typename T>
struct Serializer;

namespace detail {

template<typename T>
struct IsStdArray : std::false_type {};
template<typename T, std::size_t N>
struct IsStdArray<std::array<T, N>> : std::true_type {};

template<typename T>
struct IsVector : std::false_type {};
template<typename T, typename A>
struct IsVector<std::vector<T, A>> : std::true_type {};

template<typename T>
struct IsOptional : std::false_type {};
template<typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

template<typename T>
struct IsPair : std::false_type {};
template<typename A, typename B>
struct IsPair<std::pair<A, B>> : std::true_type {};

template<typename T>
struct IsMapLike : std::false_type {};
template<typename K, typename V, typename C, typename A>
struct IsMapLike<std::map<K, V, C, A>> : std::true_type {};
template<typename K, typename V, typename H, typename E, typename A>
struct IsMapLike<std::unordered_map<K, V, H, E, A>> : std::true_type {};

// Counting aggregate fields: AnyType converts to anything, so T{AnyType...} compiles for every N up to the
// number of fields.
struct AnyType
{
    template<typename U>
    operator U() const;
};

template<typename T, std::size_t... I>
constexpr bool BraceInitializable(std::index_sequence<I...>)
{
    return requires { T{(static_cast<void>(I), AnyType{})...}; };
}

template<typename T, std::size_t N = 0>
constexpr std::size_t CountFields()
{
    if constexpr (N < 16 && BraceInitializable<T>(std::make_index_sequence<N + 1>{}))
    {
        return CountFields<T, N + 1>();
    }
    else
    {
        return N;
    }
}

template<typename T>
auto Fields(T& value)
{
    constexpr std::size_t count = CountFields<std::remove_cv_t<T>>();
    if constexpr (count == 0) { return std::tie(); }
    else if constexpr (count == 1) { auto& [a] = value; return std::tie(a); }
    else if constexpr (count == 2) { auto& [a, b] = value; return std::tie(a, b); }
    else if constexpr (count == 3) { auto& [a, b, c] = value; return std::tie(a, b, c); }
    else if constexpr (count == 4) { auto& [a, b, c, d] = value; return std::tie(a, b, c, d); }
    else if constexpr (count == 5) { auto& [a, b, c, d, e] = value; return std::tie(a, b, c, d, e); }
    else if constexpr (count == 6) { auto& [a, b, c, d, e, f] = value; return std::tie(a, b, c, d, e, f); }
    else if constexpr (count == 7) { auto& [a, b, c, d, e, f, g] = value; return std::tie(a, b, c, d, e, f, g); }
    else if constexpr (count == 8) { auto& [a, b, c, d, e, f, g, h] = value; return std::tie(a, b, c, d, e, f, g, h); }
    else if constexpr (count == 9) { auto& [a, b, c, d, e, f, g, h, i] = value; return std::tie(a, b, c, d, e, f, g, h, i); }
    else if constexpr (count == 10) { auto& [a, b, c, d, e, f, g, h, i, j] = value; return std::tie(a, b, c, d, e, f, g, h, i, j); }
    else if constexpr (count == 11) { auto& [a, b, c, d, e, f, g, h, i, j, k] = value; return std::tie(a, b, c, d, e, f, g, h, i, j, k); }
    else if constexpr (count == 12) { auto& [a, b, c, d, e, f, g, h, i, j, k, l] = value; return std::tie(a, b, c, d, e, f, g, h, i, j, k, l); }
    else if constexpr (count == 13) { auto& [a, b, c, d, e, f, g, h, i, j, k, l, m] = value; return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m); }
    else if constexpr (count == 14) { auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n] = value; return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n); }
    else if constexpr (count == 15) { auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o] = value; return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o); }
    else { auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p] = value; return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p); }
}

template<typename T>
void WriteValue(BinaryWriter& writer, const T& value);

template<typename T>
T ReadValue(BinaryReader& reader);

} // namespace detail

template<typename T>
struct Serializer
{
    static void Write(BinaryWriter& writer, const T& value)
    {
        if constexpr (std::is_arithmetic_v<T>)
        {
            writer.Write(value);
        }
        else if constexpr (std::is_enum_v<T>)
        {
            writer.Write(static_cast<std::underlying_type_t<T>>(value));
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            writer.Write(value);
        }
        else if constexpr (detail::IsVector<T>::value)
        {
            writer.Write(static_cast<std::uint32_t>(value.size()));
            for (const auto& element : value)
            {
                detail::WriteValue(writer, element);
            }
        }
        else if constexpr (detail::IsStdArray<T>::value)
        {
            for (const auto& element : value)
            {
                detail::WriteValue(writer, element);
            }
        }
        else if constexpr (detail::IsOptional<T>::value)
        {
            writer.Write(value.has_value());
            if (value.has_value())
            {
                detail::WriteValue(writer, *value);
            }
        }
        else if constexpr (detail::IsPair<T>::value)
        {
            detail::WriteValue(writer, value.first);
            detail::WriteValue(writer, value.second);
        }
        else if constexpr (detail::IsMapLike<T>::value)
        {
            writer.Write(static_cast<std::uint32_t>(value.size()));
            for (const auto& [key, mapped] : value)
            {
                detail::WriteValue(writer, key);
                detail::WriteValue(writer, mapped);
            }
        }
        else if constexpr (std::is_aggregate_v<T>)
        {
            std::apply([&](const auto&... field) { (detail::WriteValue(writer, field), ...); },
                       detail::Fields(value));
        }
        else
        {
            static_assert(std::is_aggregate_v<T>,
                          "steamnet: no serializer for this type. Specialize steamnet::Serializer<T>.");
        }
    }

    static T Read(BinaryReader& reader)
    {
        if constexpr (std::is_arithmetic_v<T>)
        {
            return reader.Read<T>();
        }
        else if constexpr (std::is_enum_v<T>)
        {
            return static_cast<T>(reader.Read<std::underlying_type_t<T>>());
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            return reader.ReadString();
        }
        else if constexpr (detail::IsVector<T>::value)
        {
            T result;
            const std::size_t count = reader.ReadUInt32();
            // Every element takes at least one byte on the wire, so a larger count is malformed.
            if (count > reader.Remaining())
            {
                reader.Fail();
                return result;
            }
            result.reserve(count);
            for (std::size_t i = 0; i < count && reader.Ok(); ++i)
            {
                result.push_back(detail::ReadValue<typename T::value_type>(reader));
            }
            return result;
        }
        else if constexpr (detail::IsStdArray<T>::value)
        {
            T result{};
            for (auto& element : result)
            {
                element = detail::ReadValue<typename T::value_type>(reader);
            }
            return result;
        }
        else if constexpr (detail::IsOptional<T>::value)
        {
            if (reader.ReadBool())
            {
                return T{detail::ReadValue<typename T::value_type>(reader)};
            }
            return T{};
        }
        else if constexpr (detail::IsPair<T>::value)
        {
            auto first = detail::ReadValue<typename T::first_type>(reader);
            auto second = detail::ReadValue<typename T::second_type>(reader);
            return T{std::move(first), std::move(second)};
        }
        else if constexpr (detail::IsMapLike<T>::value)
        {
            T result;
            const std::size_t count = reader.ReadUInt32();
            if (count > reader.Remaining())
            {
                reader.Fail();
                return result;
            }
            for (std::size_t i = 0; i < count && reader.Ok(); ++i)
            {
                auto key = detail::ReadValue<typename T::key_type>(reader);
                auto mapped = detail::ReadValue<typename T::mapped_type>(reader);
                result.emplace(std::move(key), std::move(mapped));
            }
            return result;
        }
        else if constexpr (std::is_aggregate_v<T>)
        {
            T result{};
            std::apply([&](auto&... field)
                       { ((field = detail::ReadValue<std::remove_cvref_t<decltype(field)>>(reader)), ...); },
                       detail::Fields(result));
            return result;
        }
        else
        {
            static_assert(std::is_aggregate_v<T>,
                          "steamnet: no serializer for this type. Specialize steamnet::Serializer<T>.");
        }
    }
};

namespace detail {

template<typename T>
void WriteValue(BinaryWriter& writer, const T& value)
{
    Serializer<T>::Write(writer, value);
}

template<typename T>
T ReadValue(BinaryReader& reader)
{
    return Serializer<T>::Read(reader);
}

} // namespace detail

} // namespace steamnet
