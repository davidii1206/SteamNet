#pragma once

#include "steamnet/Session/Lobby.hpp"
#include "steamnet/Types.hpp"

#include <steam/steam_api.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace steamnet {

// Steam lobbies are used for discovery and invitations only; the game session itself runs over
// ISteamNetworkingSockets connections to the fixed host.
class LobbyManager
{
public:
    struct Callbacks
    {
        std::function<void(bool ok, std::uint64_t lobby)> onCreated;
        std::function<void(const std::vector<LobbyInfo>&)> onListReceived;
        std::function<void(bool ok, std::uint64_t lobby)> onJoined;
        // The user accepted an invitation (or "Join game") from the Steam overlay / friends list.
        std::function<void(std::uint64_t lobby)> onJoinRequested;
    };

    LobbyManager(std::string gameVersion, Callbacks callbacks);

    void Create(LobbyVisibility visibility, int maxMembers, const LobbyOptions& options, PlayerID host);
    void RequestList(const LobbyFilter& filter);
    void Join(std::uint64_t lobby);
    void Leave();
    void OpenInviteOverlay();
    void SetJoinable(bool joinable);

    std::uint64_t Current() const noexcept { return lobby_; }
    bool IsMember(PlayerID player) const;

    // Host id and compatibility metadata published in the lobby. Empty/0 when absent.
    PlayerID HostOf(std::uint64_t lobby) const;
    bool IsCompatible(std::uint64_t lobby) const;

private:
    void OnLobbyCreated(LobbyCreated_t* result, bool ioFailure);
    void OnLobbyMatchList(LobbyMatchList_t* result, bool ioFailure);
    void OnLobbyEntered(LobbyEnter_t* result, bool ioFailure);
    STEAM_CALLBACK(LobbyManager, OnJoinRequested, GameLobbyJoinRequested_t);

    std::string gameVersion_;
    Callbacks callbacks_;
    std::uint64_t lobby_ = 0;

    // Metadata of the lobby being created, applied once Steam returns its id.
    LobbyOptions pendingOptions_;
    PlayerID pendingHost_ = InvalidPlayer;
    int pendingMaxMembers_ = 0;

    CCallResult<LobbyManager, LobbyCreated_t> createCall_;
    CCallResult<LobbyManager, LobbyMatchList_t> listCall_;
    CCallResult<LobbyManager, LobbyEnter_t> enterCall_;
};

} // namespace steamnet
