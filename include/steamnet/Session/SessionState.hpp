#pragma once

#include "steamnet/Types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace steamnet {

// Chosen by the host in Network::PrepareGame and delivered to every client's OnPrepareGame callback.
struct GameStartInfo
{
    std::string map{};
    std::string gameMode{};
    std::uint64_t seed = 0;
    std::vector<std::uint8_t> customData{}; // opaque to the library
};

} // namespace steamnet
