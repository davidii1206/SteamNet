#pragma once

#include <steam/steam_api.h>

#include <cstdint>

namespace steamnet
{

// CSteamID's integer constructors are ambiguous for std::uint64_t on platforms where uint64 is a different
// type, so build it from the raw value explicitly.
inline CSteamID ToSteamID(std::uint64_t value)
{
    CSteamID id;
    id.SetFromUint64(value);
    return id;
}

} // namespace steamnet
