#pragma once

#include <cstdint>

namespace steamnet {

// A player is identified by the SteamID64 of their Steam account. The host derives the sender of every
// packet from the authenticated Steam connection, never from packet contents.
using PlayerID = std::uint64_t;
using NetEntityID = std::uint64_t;
using ComponentTypeID = std::uint16_t;
using MessageTypeID = std::uint16_t;

inline constexpr PlayerID InvalidPlayer = 0;
inline constexpr NetEntityID InvalidNetEntity = 0;

// Bumped whenever the wire format of the library itself changes.
inline constexpr std::uint32_t ProtocolVersion = 1;

enum class Authority : std::uint8_t
{
    Host,         // Only the host authors this component.
    OwningClient, // The owning client may submit updates, which the host validates.
};

enum class Audience : std::uint8_t
{
    Everyone,
    OwnerOnly,
    HostOnly, // Never sent to clients (e.g. client input).
};

enum class Delivery : std::uint8_t
{
    Reliable,
    Unreliable,
};

enum class UpdateMode : std::uint8_t
{
    OnChange, // Sent when the component is patched/replaced or marked dirty.
    Periodic, // Polled at a fixed rate; unchanged values are only refreshed occasionally.
};

enum class LobbyVisibility : std::uint8_t
{
    Public,      // k_ELobbyTypePublic: shows up in lobby search.
    FriendsOnly, // k_ELobbyTypeFriendsOnly: friends and invitees.
    InviteOnly,  // k_ELobbyTypePrivate: invitation required.
};

enum class SessionState : std::uint8_t
{
    Offline,
    Searching,
    Joining,
    InLobby,
    Connecting,
    PreparingGame,
    InGame,
    Leaving,
    Error,
};

enum class SessionEndReason : std::uint8_t
{
    HostDisconnected,
    HostEndedGame,
    ConnectionLost,
    Kicked,
    ProtocolMismatch,
    NetworkError,
};

} // namespace steamnet
