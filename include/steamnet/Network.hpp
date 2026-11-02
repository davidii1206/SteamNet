#pragma once

#include "steamnet/Config.hpp"
#include "steamnet/Messages/MessageRegistry.hpp"
#include "steamnet/Replication/Replication.hpp"
#include "steamnet/Serialization/Serializer.hpp"
#include "steamnet/Session/Lobby.hpp"
#include "steamnet/Session/Player.hpp"
#include "steamnet/Session/SessionState.hpp"
#include "steamnet/Transport/Packet.hpp"
#include "steamnet/Types.hpp"

#include <entt/entity/registry.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace steamnet {

// Steam-only, EnTT-integrated, host-authoritative networking.
//
//   entt::registry registry;
//   steamnet::Network network(registry);
//   network.Initialize();
//   network.Replication().Register<Health>({...});
//   network.CreateLobby(steamnet::LobbyVisibility::Public);
//   while (running) { network.Poll(); game.Update(dt); network.Flush(dt); }
//
// Everything must be called from one thread (the one that pumps Poll/Flush).
class Network
{
public:
    explicit Network(entt::registry& registry, NetworkConfig config = {});
    ~Network();

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    // ---- Lifecycle ----------------------------------------------------------------------------------------

    // Initializes Steamworks (unless NetworkConfig::initializeSteam is false) and the networking stack.
    bool Initialize();
    void Shutdown();
    bool IsInitialized() const noexcept;
    const std::string& LastError() const noexcept;

    // ---- Frame loop ---------------------------------------------------------------------------------------

    // Processes Steam callbacks, connection events and incoming packets (client input, RPCs, replication).
    void Poll();
    // Collects component changes and sends them. Call after the simulation so that clients receive the state
    // that already reflects the inputs processed this frame.
    void Flush(float deltaTime);
    // Poll() followed by Flush(); convenient when no simulation step has to run in between.
    void Update(float deltaTime);

    // ---- Lobbies ------------------------------------------------------------------------------------------

    // Requests the public lobby list; the result arrives via OnLobbyList and LobbyResults().
    bool FindPublicLobbies(const LobbyFilter& filter = {});
    // Creates a lobby and becomes the host. The host stays the authority for the whole session.
    bool CreateLobby(LobbyVisibility visibility, const LobbyOptions& options = {});
    bool JoinLobby(std::uint64_t lobbyId);
    // Handles "+connect_lobby <id>" so that accepting an invite while the game was closed joins the lobby.
    bool JoinLobbyFromCommandLine(int argc, const char* const* argv);
    // Opens the Steam overlay's invite dialog for the current lobby.
    void OpenInviteOverlay();
    const std::vector<LobbyInfo>& LobbyResults() const noexcept;
    std::uint64_t LobbyId() const noexcept;

    // ---- Session ------------------------------------------------------------------------------------------

    SessionState State() const noexcept;
    bool IsHost() const noexcept;
    bool InSession() const noexcept;
    PlayerID LocalPlayer() const noexcept;
    PlayerID HostPlayer() const noexcept;
    const std::vector<PlayerInfo>& Players() const noexcept;

    void SetReady(bool ready);
    bool AllPlayersReady() const;
    // Host: sends PREPARE_GAME to everyone. Clients load, then call ConfirmGameLoaded(). When all players have
    // confirmed the host sends START_GAME.
    bool PrepareGame(const GameStartInfo& info = {});
    void ConfirmGameLoaded();

    void KickPlayer(PlayerID player);
    // Leaves the session. A host ends it for everybody. OnSessionEnded is not raised for this call.
    void LeaveSession();

    // ---- Session callbacks --------------------------------------------------------------------------------

    void OnSessionEnded(std::function<void(SessionEndReason)> callback);
    void OnStateChanged(std::function<void(SessionState)> callback);
    void OnLobbyList(std::function<void(const std::vector<LobbyInfo>&)> callback);
    void OnPlayersChanged(std::function<void(const std::vector<PlayerInfo>&)> callback);
    void OnPlayerJoined(std::function<void(const PlayerInfo&)> callback);
    void OnPlayerLeft(std::function<void(PlayerID)> callback);
    void OnPrepareGame(std::function<void(const GameStartInfo&)> callback);
    void OnGameStart(std::function<void()> callback);

    // ---- ECS replication ----------------------------------------------------------------------------------

    steamnet::Replication& Replication() noexcept { return replication_; }

    // Host only. Creates an entity replicated to every client; the owner defaults to the host.
    entt::entity Spawn(PlayerID owner = InvalidPlayer) { return replication_.Spawn(owner); }
    // Host only. Opts an existing entity into replication.
    NetEntityID CreateNetworkEntity(entt::entity entity, PlayerID owner = InvalidPlayer)
    {
        return replication_.CreateNetworkEntity(entity, owner);
    }
    bool Destroy(entt::entity entity) { return replication_.Destroy(entity); }

    // Mutable access that marks the component dirty. Alternative to registry.patch<T>().
    template<typename T>
    T& Edit(entt::entity entity)
    {
        return replication_.Edit<T>(entity);
    }

    // ---- Messages / RPC -----------------------------------------------------------------------------------

    MessageRegistry& Messages() noexcept { return messages_; }

    // Client -> host. When called on the host the handler runs immediately with the local player as sender.
    template<typename T>
    bool SendToHost(const T& message)
    {
        BinaryWriter writer;
        Delivery delivery{};
        if (!EncodeMessage(message, writer, delivery))
        {
            return false;
        }
        return SendToHostBytes(writer.Bytes(), delivery);
    }

    // Host -> every client.
    template<typename T>
    bool Broadcast(const T& message)
    {
        BinaryWriter writer;
        Delivery delivery{};
        if (!EncodeMessage(message, writer, delivery))
        {
            return false;
        }
        return BroadcastBytes(writer.Bytes(), delivery);
    }

    // Host -> one client. Clients cannot address each other; the host relays when a game needs it.
    template<typename T>
    bool SendTo(PlayerID player, const T& message)
    {
        BinaryWriter writer;
        Delivery delivery{};
        if (!EncodeMessage(message, writer, delivery))
        {
            return false;
        }
        return SendToBytes(player, writer.Bytes(), delivery);
    }

private:
    class Impl;

    template<typename T>
    bool EncodeMessage(const T& message, BinaryWriter& writer, Delivery& delivery) const
    {
        writer.Write(static_cast<std::uint8_t>(PacketType::UserMessage));
        return messages_.Encode(message, writer, delivery);
    }

    bool SendToHostBytes(std::span<const std::uint8_t> bytes, Delivery delivery);
    bool BroadcastBytes(std::span<const std::uint8_t> bytes, Delivery delivery);
    bool SendToBytes(PlayerID player, std::span<const std::uint8_t> bytes, Delivery delivery);

    NetworkConfig config_;
    steamnet::Replication replication_;
    MessageRegistry messages_;
    std::unique_ptr<Impl> impl_;
};

} // namespace steamnet
