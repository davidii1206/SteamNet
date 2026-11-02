#pragma once

#include "steamnet/Types.hpp"

#include <cstdint>

namespace steamnet {

// First byte of every packet on the wire.
enum class PacketType : std::uint8_t
{
    // Handshake / session
    Hello = 1,    // client -> host: protocol version, game version, schema fingerprint
    Welcome,      // host -> client: accepted, includes current lobby state
    Reject,       // host -> client: refused, includes reason
    LobbyState,   // host -> clients: player list and ready flags
    SetReady,     // client -> host
    PrepareGame,  // host -> clients
    GameLoaded,   // client -> host
    StartGame,    // host -> clients
    SessionEnd,   // either direction: orderly shutdown with reason
    Kick,         // host -> client

    // Replication
    ReplicationBatch = 32, // a series of ReplicationOp entries (host -> client, or client -> host updates)
    SnapshotEnd,           // host -> client: baseline complete
    SnapshotAck,           // client -> host: baseline applied

    // Messages
    UserMessage = 64,
};

enum class ReplicationOp : std::uint8_t
{
    CreateEntity = 1,
    DestroyEntity,
    AddComponent,
    UpdateComponent,
    RemoveComponent,
    SetOwner,
};

enum class RejectReason : std::uint8_t
{
    ProtocolMismatch = 1,
    GameVersionMismatch,
    SchemaMismatch,
    SessionFull,
    InProgress,
};

} // namespace steamnet
