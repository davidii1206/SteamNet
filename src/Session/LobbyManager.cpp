#include "LobbyManager.hpp"

#include "SteamUtil.hpp"
#include "steamnet/Types.hpp"

#include <cstdlib>
#include <utility>

namespace steamnet {

namespace {

constexpr const char* KeyProtocol = "steamnet_protocol";
constexpr const char* KeyVersion = "steamnet_version";
constexpr const char* KeyHost = "steamnet_host";
constexpr const char* KeyName = "name";
constexpr const char* KeyMode = "mode";
constexpr const char* KeyMap = "map";

ELobbyType ToSteam(LobbyVisibility visibility)
{
    switch (visibility)
    {
    case LobbyVisibility::Public:
        return k_ELobbyTypePublic;
    case LobbyVisibility::FriendsOnly:
        return k_ELobbyTypeFriendsOnly;
    case LobbyVisibility::InviteOnly:
        return k_ELobbyTypePrivate;
    }
    return k_ELobbyTypePrivate;
}

} // namespace

LobbyManager::LobbyManager(std::string gameVersion, Callbacks callbacks)
    : gameVersion_(std::move(gameVersion)), callbacks_(std::move(callbacks))
{
}

void LobbyManager::Create(LobbyVisibility visibility, int maxMembers, const LobbyOptions& options, PlayerID host)
{
    pendingOptions_ = options;
    pendingHost_ = host;
    pendingMaxMembers_ = maxMembers;
    const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(ToSteam(visibility), maxMembers);
    createCall_.Set(call, this, &LobbyManager::OnLobbyCreated);
}

void LobbyManager::OnLobbyCreated(LobbyCreated_t* result, bool ioFailure)
{
    if (ioFailure || result->m_eResult != k_EResultOK)
    {
        if (callbacks_.onCreated)
        {
            callbacks_.onCreated(false, 0);
        }
        return;
    }

    lobby_ = result->m_ulSteamIDLobby;
    const CSteamID id = ToSteamID(lobby_);
    ISteamMatchmaking* matchmaking = SteamMatchmaking();

    std::string name = pendingOptions_.name;
    if (name.empty())
    {
        name = std::string(SteamFriends()->GetPersonaName()) + "'s game";
    }

    matchmaking->SetLobbyData(id, KeyProtocol, std::to_string(ProtocolVersion).c_str());
    matchmaking->SetLobbyData(id, KeyVersion, gameVersion_.c_str());
    matchmaking->SetLobbyData(id, KeyHost, std::to_string(pendingHost_).c_str());
    matchmaking->SetLobbyData(id, KeyName, name.c_str());
    matchmaking->SetLobbyData(id, KeyMode, pendingOptions_.gameMode.c_str());
    matchmaking->SetLobbyData(id, KeyMap, pendingOptions_.map.c_str());
    matchmaking->SetLobbyMemberLimit(id, pendingMaxMembers_);

    if (callbacks_.onCreated)
    {
        callbacks_.onCreated(true, lobby_);
    }
}

void LobbyManager::RequestList(const LobbyFilter& filter)
{
    ISteamMatchmaking* matchmaking = SteamMatchmaking();
    matchmaking->AddRequestLobbyListStringFilter(KeyProtocol, std::to_string(ProtocolVersion).c_str(), k_ELobbyComparisonEqual);
    matchmaking->AddRequestLobbyListStringFilter(KeyVersion, gameVersion_.c_str(), k_ELobbyComparisonEqual);
    if (!filter.gameMode.empty())
    {
        matchmaking->AddRequestLobbyListStringFilter(KeyMode, filter.gameMode.c_str(), k_ELobbyComparisonEqual);
    }
    matchmaking->AddRequestLobbyListDistanceFilter(filter.worldwide ? k_ELobbyDistanceFilterWorldwide
                                                                    : k_ELobbyDistanceFilterDefault);
    matchmaking->AddRequestLobbyListResultCountFilter(filter.maxResults);
    listCall_.Set(matchmaking->RequestLobbyList(), this, &LobbyManager::OnLobbyMatchList);
}

void LobbyManager::OnLobbyMatchList(LobbyMatchList_t* result, bool ioFailure)
{
    std::vector<LobbyInfo> lobbies;
    if (!ioFailure)
    {
        ISteamMatchmaking* matchmaking = SteamMatchmaking();
        for (std::uint32_t i = 0; i < result->m_nLobbiesMatching; ++i)
        {
            const CSteamID id = matchmaking->GetLobbyByIndex(static_cast<int>(i));
            LobbyInfo info;
            info.id = id.ConvertToUint64();
            info.name = matchmaking->GetLobbyData(id, KeyName);
            info.gameMode = matchmaking->GetLobbyData(id, KeyMode);
            info.map = matchmaking->GetLobbyData(id, KeyMap);
            info.players = matchmaking->GetNumLobbyMembers(id);
            info.maxPlayers = matchmaking->GetLobbyMemberLimit(id);
            info.joinable = info.players < info.maxPlayers && HostOf(info.id) != InvalidPlayer;
            lobbies.push_back(std::move(info));
        }
    }
    if (callbacks_.onListReceived)
    {
        callbacks_.onListReceived(lobbies);
    }
}

void LobbyManager::Join(std::uint64_t lobby)
{
    enterCall_.Set(SteamMatchmaking()->JoinLobby(ToSteamID(lobby)), this, &LobbyManager::OnLobbyEntered);
}

void LobbyManager::OnLobbyEntered(LobbyEnter_t* result, bool ioFailure)
{
    const bool ok = !ioFailure && result->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess;
    if (ok)
    {
        lobby_ = result->m_ulSteamIDLobby;
    }
    if (callbacks_.onJoined)
    {
        callbacks_.onJoined(ok, ok ? lobby_ : result->m_ulSteamIDLobby);
    }
}

void LobbyManager::OnJoinRequested(GameLobbyJoinRequested_t* request)
{
    if (callbacks_.onJoinRequested)
    {
        callbacks_.onJoinRequested(request->m_steamIDLobby.ConvertToUint64());
    }
}

void LobbyManager::Leave()
{
    if (lobby_ != 0)
    {
        SteamMatchmaking()->LeaveLobby(ToSteamID(lobby_));
        lobby_ = 0;
    }
}

void LobbyManager::OpenInviteOverlay()
{
    if (lobby_ != 0)
    {
        SteamFriends()->ActivateGameOverlayInviteDialog(ToSteamID(lobby_));
    }
}

void LobbyManager::SetJoinable(bool joinable)
{
    if (lobby_ != 0)
    {
        SteamMatchmaking()->SetLobbyJoinable(ToSteamID(lobby_), joinable);
    }
}

bool LobbyManager::IsMember(PlayerID player) const
{
    if (lobby_ == 0)
    {
        return false;
    }
    ISteamMatchmaking* matchmaking = SteamMatchmaking();
    const CSteamID id = ToSteamID(lobby_);
    const int count = matchmaking->GetNumLobbyMembers(id);
    for (int i = 0; i < count; ++i)
    {
        if (matchmaking->GetLobbyMemberByIndex(id, i).ConvertToUint64() == player)
        {
            return true;
        }
    }
    return false;
}

PlayerID LobbyManager::HostOf(std::uint64_t lobby) const
{
    const char* value = SteamMatchmaking()->GetLobbyData(ToSteamID(lobby), KeyHost);
    return value != nullptr ? std::strtoull(value, nullptr, 10) : InvalidPlayer;
}

bool LobbyManager::IsCompatible(std::uint64_t lobby) const
{
    ISteamMatchmaking* matchmaking = SteamMatchmaking();
    const CSteamID id = ToSteamID(lobby);
    return std::to_string(ProtocolVersion) == matchmaking->GetLobbyData(id, KeyProtocol) &&
           gameVersion_ == matchmaking->GetLobbyData(id, KeyVersion);
}

} // namespace steamnet
