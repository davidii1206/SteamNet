#pragma once

#include "steamnet/Replication/Replication.hpp"

#include <cstdint>
#include <string>

namespace steamnet {

struct NetworkConfig
{
    // Passed to SteamAPI_RestartAppIfNecessary when non-zero. Leave at 0 while developing with steam_appid.txt.
    std::uint32_t appId = 0;

    // true: the library calls SteamAPI_Init / SteamAPI_Shutdown. false: the game already initialized Steamworks.
    bool initializeSteam = true;

    // true: Poll() calls SteamAPI_RunCallbacks. false: the game pumps Steam callbacks itself (once per frame).
    bool runSteamCallbacks = true;

    // Lobbies and handshakes with a different game version are refused.
    std::string gameVersion = "1";

    std::uint32_t maxPlayers = 4;

    // Allow clients to join after PrepareGame. They receive the world snapshot and the PrepareGame callback.
    bool allowJoinInProgress = false;

    // Seconds a connection may take to finish the handshake before it is dropped.
    float handshakeTimeout = 15.0f;

    ReplicationConfig replication;
};

} // namespace steamnet
