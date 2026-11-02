#pragma once

#include "steamnet/Types.hpp"

#include <steam/steam_api.h>
#include <steam/isteamnetworkingsockets.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace steamnet {

// Thin wrapper over ISteamNetworkingSockets: one P2P (Steam Datagram Relay capable) connection per remote
// player. The remote PlayerID is taken from the authenticated Steam identity of the connection.
class SteamTransport
{
public:
    enum class EventType : std::uint8_t
    {
        Incoming,     // host: a peer is connecting (already accepted or refused via the filter)
        Connected,    // the connection is usable
        ClosedByPeer, // the remote side closed the connection
        Lost,         // local problem / timeout
    };

    struct Event
    {
        EventType type;
        PlayerID player;
    };

    struct Packet
    {
        PlayerID from;
        std::span<const std::uint8_t> data;
    };

    SteamTransport();
    ~SteamTransport();

    SteamTransport(const SteamTransport&) = delete;
    SteamTransport& operator=(const SteamTransport&) = delete;

    bool Listen();
    bool Connect(PlayerID host);
    // Closes everything. Pending reliable data is flushed first when `linger` is true.
    void CloseAll(bool linger);
    void Disconnect(PlayerID player, bool linger);

    bool Send(PlayerID to, std::span<const std::uint8_t> data, Delivery delivery);
    bool IsConnected(PlayerID player) const;

    // Pumps Steam's connection callbacks and appends the resulting events.
    void PumpEvents(std::vector<Event>& events);
    // Receives all queued packets; `handler(PlayerID, span)`.
    template<typename Handler>
    void Receive(Handler&& handler)
    {
        constexpr int maxBatch = 64;
        SteamNetworkingMessage_t* messages[maxBatch];
        for (;;)
        {
            const int count = sockets_->ReceiveMessagesOnPollGroup(pollGroup_, messages, maxBatch);
            if (count <= 0)
            {
                break;
            }
            for (int i = 0; i < count; ++i)
            {
                SteamNetworkingMessage_t* message = messages[i];
                if (const auto found = byHandle_.find(message->m_conn); found != byHandle_.end())
                {
                    handler(found->second,
                            std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(message->m_pData),
                                                          static_cast<std::size_t>(message->m_cbSize)));
                }
                message->Release();
            }
        }
    }

private:
    static void OnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info);
    void OnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
    void Track(HSteamNetConnection connection, PlayerID player);
    void Forget(HSteamNetConnection connection);

    ISteamNetworkingSockets* sockets_ = nullptr;
    HSteamListenSocket listenSocket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup pollGroup_ = k_HSteamNetPollGroup_Invalid;

    std::unordered_map<PlayerID, HSteamNetConnection> connections_;
    std::unordered_map<HSteamNetConnection, PlayerID> byHandle_;
    std::vector<Event> pendingEvents_;

    static SteamTransport* active_;
};

} // namespace steamnet
