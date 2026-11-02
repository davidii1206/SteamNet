#pragma once

#include "steamnet/Types.hpp"

#include <cstdint>
#include <string>

namespace steamnet {

// One entry of the public lobby browser.
struct LobbyInfo
{
    std::uint64_t id = 0;
    std::string name{};
    std::string gameMode{};
    std::string map{};
    int players = 0;
    int maxPlayers = 0;
    bool joinable = false;
};

// Metadata published with a lobby so that the browser can show and filter it.
struct LobbyOptions
{
    std::string name{}; // defaults to "<your Steam name>'s game"
    std::string gameMode{};
    std::string map{};
};

// Restricts what FindPublicLobbies returns. Lobbies with an incompatible protocol or game version are always
// filtered out.
struct LobbyFilter
{
    std::string gameMode{}; // empty: any
    int maxResults = 50;
    bool worldwide = false; // false: Steam's default distance filter
};

} // namespace steamnet
