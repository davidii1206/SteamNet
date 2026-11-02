#include "SteamTransport.hpp"

#include <steam/isteamnetworkingutils.h>

namespace steamnet {

SteamTransport* SteamTransport::active_ = nullptr;

SteamTransport::SteamTransport()
{
    sockets_ = SteamNetworkingSockets();
    pollGroup_ = sockets_->CreatePollGroup();
    active_ = this;

    // Start talking to the Steam relay network early so that the first connection is fast.
    SteamNetworkingUtils()->InitRelayNetworkAccess();
}

SteamTransport::~SteamTransport()
{
    CloseAll(false);
    if (pollGroup_ != k_HSteamNetPollGroup_Invalid)
    {
        sockets_->DestroyPollGroup(pollGroup_);
    }
    if (active_ == this)
    {
        active_ = nullptr;
    }
}

bool SteamTransport::Listen()
{
    if (listenSocket_ != k_HSteamListenSocket_Invalid)
    {
        return true;
    }
    SteamNetworkingConfigValue_t option;
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                  reinterpret_cast<void*>(&SteamTransport::OnStatusChangedStatic));
    listenSocket_ = sockets_->CreateListenSocketP2P(0, 1, &option);
    return listenSocket_ != k_HSteamListenSocket_Invalid;
}

bool SteamTransport::Connect(PlayerID host)
{
    SteamNetworkingIdentity identity;
    identity.SetSteamID64(host);

    SteamNetworkingConfigValue_t option;
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                  reinterpret_cast<void*>(&SteamTransport::OnStatusChangedStatic));
    const HSteamNetConnection connection = sockets_->ConnectP2P(identity, 0, 1, &option);
    if (connection == k_HSteamNetConnection_Invalid)
    {
        return false;
    }
    Track(connection, host);
    return true;
}

void SteamTransport::Track(HSteamNetConnection connection, PlayerID player)
{
    // One connection per player: a newer one replaces the older one.
    if (const auto existing = connections_.find(player); existing != connections_.end() && existing->second != connection)
    {
        const HSteamNetConnection stale = existing->second;
        byHandle_.erase(stale);
        sockets_->CloseConnection(stale, 0, "replaced", false);
    }
    connections_[player] = connection;
    byHandle_[connection] = player;
    sockets_->SetConnectionPollGroup(connection, pollGroup_);
}

void SteamTransport::Forget(HSteamNetConnection connection)
{
    if (const auto found = byHandle_.find(connection); found != byHandle_.end())
    {
        if (const auto player = connections_.find(found->second);
            player != connections_.end() && player->second == connection)
        {
            connections_.erase(player);
        }
        byHandle_.erase(found);
    }
}

void SteamTransport::CloseAll(bool linger)
{
    for (const auto& [player, connection] : connections_)
    {
        sockets_->CloseConnection(connection, 0, "closing", linger);
    }
    connections_.clear();
    byHandle_.clear();
    pendingEvents_.clear();

    if (listenSocket_ != k_HSteamListenSocket_Invalid)
    {
        sockets_->CloseListenSocket(listenSocket_);
        listenSocket_ = k_HSteamListenSocket_Invalid;
    }
}

void SteamTransport::Disconnect(PlayerID player, bool linger)
{
    const auto found = connections_.find(player);
    if (found == connections_.end())
    {
        return;
    }
    const HSteamNetConnection connection = found->second;
    byHandle_.erase(connection);
    connections_.erase(found);
    sockets_->CloseConnection(connection, 0, "disconnect", linger);
}

bool SteamTransport::Send(PlayerID to, std::span<const std::uint8_t> data, Delivery delivery)
{
    const auto found = connections_.find(to);
    if (found == connections_.end())
    {
        return false;
    }
    // Replication already batches per tick, so Nagle's delay would only add latency.
    const int flags = delivery == Delivery::Reliable ? k_nSteamNetworkingSend_ReliableNoNagle
                                                     : k_nSteamNetworkingSend_UnreliableNoNagle;
    return sockets_->SendMessageToConnection(found->second, data.data(), static_cast<std::uint32_t>(data.size()),
                                             flags, nullptr) == k_EResultOK;
}

bool SteamTransport::IsConnected(PlayerID player) const
{
    return connections_.contains(player);
}

void SteamTransport::PumpEvents(std::vector<Event>& events)
{
    sockets_->RunCallbacks();
    events.insert(events.end(), pendingEvents_.begin(), pendingEvents_.end());
    pendingEvents_.clear();
}

void SteamTransport::OnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info)
{
    if (active_ != nullptr)
    {
        active_->OnStatusChanged(info);
    }
}

void SteamTransport::OnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    const HSteamNetConnection connection = info->m_hConn;
    const auto state = info->m_info.m_eState;
    const bool isIncoming = info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid;
    const PlayerID remote = info->m_info.m_identityRemote.GetSteamID64();

    // Only trust connections with an authenticated Steam identity.
    if (state == k_ESteamNetworkingConnectionState_Connecting && info->m_eOldState == k_ESteamNetworkingConnectionState_None &&
        isIncoming)
    {
        if (remote == InvalidPlayer || sockets_->AcceptConnection(connection) != k_EResultOK)
        {
            sockets_->CloseConnection(connection, 0, "refused", false);
            return;
        }
        Track(connection, remote);
        pendingEvents_.push_back({EventType::Incoming, remote});
        return;
    }

    const auto known = byHandle_.find(connection);
    switch (state)
    {
    case k_ESteamNetworkingConnectionState_Connected:
        if (known != byHandle_.end())
        {
            pendingEvents_.push_back({EventType::Connected, known->second});
        }
        break;

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        if (known != byHandle_.end())
        {
            const PlayerID player = known->second;
            pendingEvents_.push_back({state == k_ESteamNetworkingConnectionState_ClosedByPeer ? EventType::ClosedByPeer
                                                                                              : EventType::Lost,
                                      player});
            Forget(connection);
        }
        // Always release the handle, including connections we already stopped tracking.
        sockets_->CloseConnection(connection, 0, nullptr, false);
        break;

    default:
        break;
    }
}

} // namespace steamnet
