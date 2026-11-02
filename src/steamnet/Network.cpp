#include "steamnet/Network.hpp"

#include "SteamUtil.hpp"
#include "Session/LobbyManager.hpp"
#include "Transport/SteamTransport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace steamnet {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float MembershipGraceSeconds = 10.0f;

Clock::time_point After(float seconds)
{
    return Clock::now() + std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000.0f));
}

std::string PersonaName(PlayerID player)
{
    const char* name = SteamFriends()->GetFriendPersonaName(ToSteamID(player));
    return name != nullptr ? name : std::string();
}

struct Hello
{
    std::uint32_t protocol = 0;
    std::string gameVersion;
    std::uint64_t replicationFingerprint = 0;
    std::uint64_t messageFingerprint = 0;
};

} // namespace

class Network::Impl
{
public:
    explicit Impl(Network& owner) : net(owner), config(owner.config_) {}

    // ---- Lifecycle ----------------------------------------------------------------------------------------

    bool Initialize()
    {
        if (initialized)
        {
            return true;
        }

        if (config.initializeSteam)
        {
            if (config.appId != 0 && SteamAPI_RestartAppIfNecessary(config.appId))
            {
                lastError = "Steam is restarting the game through the Steam client.";
                return false;
            }
            SteamErrMsg message{};
            if (SteamAPI_InitEx(&message) != k_ESteamAPIInitResult_OK)
            {
                lastError = std::string("SteamAPI_Init failed: ") + message;
                return false;
            }
            ownsSteam = true;
        }
        else if (SteamUser() == nullptr)
        {
            lastError = "Steamworks has not been initialized by the game.";
            return false;
        }

        local = SteamUser()->GetSteamID().ConvertToUint64();

        transport = std::make_unique<SteamTransport>();

        LobbyManager::Callbacks callbacks;
        callbacks.onCreated = [this](bool ok, std::uint64_t) { OnLobbyCreated(ok); };
        callbacks.onListReceived = [this](const std::vector<LobbyInfo>& lobbies)
        {
            lobbyResults = lobbies;
            if (state == SessionState::Searching)
            {
                SetState(SessionState::Offline);
            }
            if (onLobbyList)
            {
                onLobbyList(lobbyResults);
            }
        };
        callbacks.onJoined = [this](bool ok, std::uint64_t id) { OnLobbyJoined(ok, id); };
        callbacks.onJoinRequested = [this](std::uint64_t id) { OnJoinRequested(id); };
        lobby = std::make_unique<LobbyManager>(config.gameVersion, std::move(callbacks));

        initialized = true;
        state = SessionState::Offline;
        return true;
    }

    void Shutdown()
    {
        if (!initialized)
        {
            return;
        }
        Teardown(SessionEndReason::HostEndedGame, false, SessionState::Offline);
        lobby.reset();
        transport.reset();
        if (ownsSteam)
        {
            SteamAPI_Shutdown();
            ownsSteam = false;
        }
        initialized = false;
    }

    // ---- State --------------------------------------------------------------------------------------------

    void SetState(SessionState next)
    {
        if (state == next)
        {
            return;
        }
        state = next;
        if (onStateChanged)
        {
            onStateChanged(state);
        }
    }

    bool Idle() const
    {
        return state == SessionState::Offline || state == SessionState::Error || state == SessionState::Searching;
    }

    // Ends the session locally and returns to `finalState`. Raises OnSessionEnded when `notify` is set.
    void Teardown(SessionEndReason reason, bool notify, SessionState finalState)
    {
        if (!initialized)
        {
            return;
        }
        const bool wasActive = !Idle() || isHost;
        if (state == SessionState::Leaving || !wasActive)
        {
            return;
        }

        SetState(SessionState::Leaving);
        net.replication_.Stop();
        if (transport)
        {
            transport->CloseAll(true);
        }
        if (lobby)
        {
            lobby->Leave();
        }

        players.clear();
        loaded.clear();
        unidentified.clear();
        awaitingMembership.clear();
        isHost = false;
        host = InvalidPlayer;
        startInfo = {};
        SetState(finalState);

        if (notify && wasActive && onSessionEnded)
        {
            onSessionEnded(reason);
        }
    }

    void Fail(SessionEndReason reason, std::string message)
    {
        lastError = std::move(message);
        Teardown(reason, true, SessionState::Error);
    }

    // ---- Lobbies ------------------------------------------------------------------------------------------

    bool FindPublicLobbies(const LobbyFilter& filter)
    {
        if (!initialized || !Idle())
        {
            return false;
        }
        SetState(SessionState::Searching);
        lobby->RequestList(filter);
        return true;
    }

    bool CreateLobby(LobbyVisibility visibility, const LobbyOptions& options)
    {
        if (!initialized || !Idle())
        {
            return false;
        }
        isHost = true;
        host = local;
        SetState(SessionState::Joining);
        lobby->Create(visibility, static_cast<int>(config.maxPlayers), options, local);
        return true;
    }

    void OnLobbyCreated(bool ok)
    {
        if (state != SessionState::Joining || !isHost)
        {
            lobby->Leave();
            return;
        }
        if (!ok)
        {
            Fail(SessionEndReason::NetworkError, "Steam could not create the lobby.");
            return;
        }
        if (!transport->Listen())
        {
            Fail(SessionEndReason::NetworkError, "Could not open the host socket.");
            return;
        }

        PlayerInfo self;
        self.id = local;
        self.name = PersonaName(local);
        self.isHost = true;
        players = {self};
        loaded.clear();
        net.replication_.StartHost(local, MakeSender());
        SetState(SessionState::InLobby);
        NotifyPlayers();
    }

    bool JoinLobby(std::uint64_t id)
    {
        if (!initialized || !Idle())
        {
            return false;
        }
        SetState(SessionState::Joining);
        lobby->Join(id);
        return true;
    }

    void OnLobbyJoined(bool ok, std::uint64_t id)
    {
        if (state != SessionState::Joining || isHost)
        {
            if (ok)
            {
                lobby->Leave();
            }
            return;
        }
        if (!ok)
        {
            Fail(SessionEndReason::NetworkError, "Could not join the lobby (it may be full or no longer exist).");
            return;
        }
        if (!lobby->IsCompatible(id))
        {
            Fail(SessionEndReason::ProtocolMismatch, "The lobby was created by an incompatible game version.");
            return;
        }

        host = lobby->HostOf(id);
        if (host == InvalidPlayer || host == local)
        {
            Fail(SessionEndReason::NetworkError, "The lobby does not name a valid host.");
            return;
        }

        SetState(SessionState::Connecting);
        connectDeadline = After(config.handshakeTimeout);
        if (!transport->Connect(host))
        {
            Fail(SessionEndReason::NetworkError, "Could not start a connection to the host.");
        }
    }

    void OnJoinRequested(std::uint64_t id)
    {
        if (state == SessionState::InLobby || Idle())
        {
            Teardown(SessionEndReason::HostEndedGame, false, SessionState::Offline);
            JoinLobby(id);
        }
    }

    // ---- Sending ------------------------------------------------------------------------------------------

    Replication::SendFunction MakeSender()
    {
        return [this](PlayerID to, std::span<const std::uint8_t> data, Delivery delivery)
        { transport->Send(to, data, delivery); };
    }

    bool SendBytes(PlayerID to, std::span<const std::uint8_t> data, Delivery delivery)
    {
        return initialized && transport && transport->Send(to, data, delivery);
    }

    void SendPacket(PlayerID to, const BinaryWriter& writer, Delivery delivery = Delivery::Reliable)
    {
        SendBytes(to, writer.Bytes(), delivery);
    }

    void SendToClients(std::span<const std::uint8_t> bytes, Delivery delivery = Delivery::Reliable)
    {
        for (const PlayerInfo& player : players)
        {
            if (player.id != local)
            {
                SendBytes(player.id, bytes, delivery);
            }
        }
    }

    void SendToClients(const BinaryWriter& writer, Delivery delivery = Delivery::Reliable)
    {
        SendToClients(writer.Bytes(), delivery);
    }

    static void WritePlayers(BinaryWriter& writer, const std::vector<PlayerInfo>& list)
    {
        writer.Write(static_cast<std::uint8_t>(list.size()));
        for (const PlayerInfo& player : list)
        {
            writer.Write(player.id);
            writer.Write(player.name);
            writer.Write(player.ready);
        }
    }

    bool ReadPlayers(BinaryReader& reader, std::vector<PlayerInfo>& out) const
    {
        const std::size_t count = reader.ReadUInt8();
        out.clear();
        for (std::size_t i = 0; i < count && reader.Ok(); ++i)
        {
            PlayerInfo player;
            player.id = reader.ReadUInt64();
            player.name = reader.ReadString(256);
            player.ready = reader.ReadBool();
            player.isHost = player.id == host;
            out.push_back(std::move(player));
        }
        return reader.Ok();
    }

    void BroadcastLobbyState()
    {
        BinaryWriter writer;
        writer.Write(static_cast<std::uint8_t>(PacketType::LobbyState));
        WritePlayers(writer, players);
        SendToClients(writer);
    }

    void NotifyPlayers()
    {
        if (onPlayersChanged)
        {
            onPlayersChanged(players);
        }
    }

    PlayerInfo* FindPlayer(PlayerID id)
    {
        const auto found = std::find_if(players.begin(), players.end(), [id](const PlayerInfo& p) { return p.id == id; });
        return found == players.end() ? nullptr : &*found;
    }

    // ---- Frame loop ---------------------------------------------------------------------------------------

    void Poll()
    {
        if (!initialized)
        {
            return;
        }
        if (config.runSteamCallbacks)
        {
            SteamAPI_RunCallbacks();
        }

        events.clear();
        transport->PumpEvents(events);
        for (const SteamTransport::Event& event : events)
        {
            HandleEvent(event);
        }

        transport->Receive([this](PlayerID from, std::span<const std::uint8_t> data) { HandlePacket(from, data); });

        CheckTimeouts();
    }

    void CheckTimeouts()
    {
        const Clock::time_point now = Clock::now();

        if (state == SessionState::Connecting && now > connectDeadline)
        {
            Fail(SessionEndReason::NetworkError, "Timed out while connecting to the host.");
            return;
        }

        if (!isHost)
        {
            return;
        }

        std::vector<PlayerID> expired;
        for (const auto& [player, deadline] : unidentified)
        {
            if (now > deadline)
            {
                expired.push_back(player);
            }
        }
        for (const PlayerID player : expired)
        {
            unidentified.erase(player);
            transport->Disconnect(player, false);
        }

        // Retry handshakes that arrived before the lobby member list showed the player.
        std::vector<std::pair<PlayerID, Hello>> ready;
        std::vector<PlayerID> giveUp;
        for (const auto& [player, pending] : awaitingMembership)
        {
            if (lobby->IsMember(player))
            {
                ready.emplace_back(player, pending.hello);
            }
            else if (now > pending.deadline)
            {
                giveUp.push_back(player);
            }
        }
        for (const auto& [player, hello] : ready)
        {
            awaitingMembership.erase(player);
            Admit(player, hello);
        }
        for (const PlayerID player : giveUp)
        {
            awaitingMembership.erase(player);
            transport->Disconnect(player, false);
        }
    }

    void HandleEvent(const SteamTransport::Event& event)
    {
        switch (event.type)
        {
        case SteamTransport::EventType::Incoming:
            if (isHost)
            {
                unidentified[event.player] = After(config.handshakeTimeout);
            }
            else
            {
                transport->Disconnect(event.player, false);
            }
            break;

        case SteamTransport::EventType::Connected:
            if (!isHost && event.player == host && state == SessionState::Connecting)
            {
                SendHello();
            }
            break;

        case SteamTransport::EventType::ClosedByPeer:
        case SteamTransport::EventType::Lost:
            if (isHost)
            {
                unidentified.erase(event.player);
                awaitingMembership.erase(event.player);
                if (FindPlayer(event.player) != nullptr)
                {
                    RemovePlayer(event.player, false);
                }
            }
            else if (event.player == host)
            {
                if (state == SessionState::Connecting || state == SessionState::Joining)
                {
                    Fail(SessionEndReason::NetworkError, "The connection to the host could not be established.");
                }
                else
                {
                    Teardown(event.type == SteamTransport::EventType::ClosedByPeer ? SessionEndReason::HostDisconnected
                                                                                   : SessionEndReason::ConnectionLost,
                             true, SessionState::Offline);
                }
            }
            break;
        }
    }

    // ---- Handshake ----------------------------------------------------------------------------------------

    void SendHello()
    {
        BinaryWriter writer;
        writer.Write(static_cast<std::uint8_t>(PacketType::Hello));
        writer.Write(ProtocolVersion);
        writer.Write(config.gameVersion);
        writer.Write(net.replication_.Fingerprint());
        writer.Write(net.messages_.Fingerprint());
        SendPacket(host, writer);
    }

    void Reject(PlayerID player, RejectReason reason)
    {
        BinaryWriter writer;
        writer.Write(static_cast<std::uint8_t>(PacketType::Reject));
        writer.Write(static_cast<std::uint8_t>(reason));
        SendPacket(player, writer);
        transport->Disconnect(player, true);
    }

    void HandleHello(PlayerID sender, BinaryReader& reader)
    {
        if (!isHost || FindPlayer(sender) != nullptr || awaitingMembership.contains(sender))
        {
            return;
        }

        Hello hello;
        hello.protocol = reader.ReadUInt32();
        hello.gameVersion = reader.ReadString(128);
        hello.replicationFingerprint = reader.ReadUInt64();
        hello.messageFingerprint = reader.ReadUInt64();
        if (!reader.Ok())
        {
            transport->Disconnect(sender, false);
            return;
        }

        unidentified.erase(sender);

        // Invite-only and friends-only lobbies must stay closed: only actual lobby members may play, even if
        // someone knows the host's Steam id.
        if (!lobby->IsMember(sender))
        {
            awaitingMembership[sender] = {hello, After(MembershipGraceSeconds)};
            return;
        }
        Admit(sender, hello);
    }

    void Admit(PlayerID sender, const Hello& hello)
    {
        if (hello.protocol != ProtocolVersion)
        {
            Reject(sender, RejectReason::ProtocolMismatch);
            return;
        }
        if (hello.gameVersion != config.gameVersion)
        {
            Reject(sender, RejectReason::GameVersionMismatch);
            return;
        }
        if (hello.replicationFingerprint != net.replication_.Fingerprint() ||
            hello.messageFingerprint != net.messages_.Fingerprint())
        {
            Reject(sender, RejectReason::SchemaMismatch);
            return;
        }
        if (players.size() >= config.maxPlayers)
        {
            Reject(sender, RejectReason::SessionFull);
            return;
        }
        if ((state == SessionState::PreparingGame || state == SessionState::InGame) && !config.allowJoinInProgress)
        {
            Reject(sender, RejectReason::InProgress);
            return;
        }
        if (state != SessionState::InLobby && state != SessionState::PreparingGame && state != SessionState::InGame)
        {
            Reject(sender, RejectReason::InProgress);
            return;
        }

        PlayerInfo info;
        info.id = sender;
        info.name = PersonaName(sender);
        players.push_back(info);
        loaded[sender] = false;

        BinaryWriter welcome;
        welcome.Write(static_cast<std::uint8_t>(PacketType::Welcome));
        welcome.Write(local);
        WritePlayers(welcome, players);
        SendPacket(sender, welcome);

        // Welcome first, then the world: both travel on the same ordered reliable stream.
        net.replication_.AddPeer(sender);

        if (state == SessionState::PreparingGame || state == SessionState::InGame)
        {
            SendPrepareGame(sender);
        }

        BroadcastLobbyState();
        if (onPlayerJoined)
        {
            onPlayerJoined(info);
        }
        NotifyPlayers();
    }

    void RemovePlayer(PlayerID player, bool linger)
    {
        const auto found = std::find_if(players.begin(), players.end(), [player](const PlayerInfo& p) { return p.id == player; });
        if (found == players.end() || player == local)
        {
            return;
        }
        players.erase(found);
        loaded.erase(player);
        net.replication_.RemovePeer(player);
        transport->Disconnect(player, linger);

        BroadcastLobbyState();
        if (onPlayerLeft)
        {
            onPlayerLeft(player);
        }
        NotifyPlayers();
        TryStartGame();
    }

    // ---- Packets ------------------------------------------------------------------------------------------

    void HandlePacket(PlayerID sender, std::span<const std::uint8_t> data)
    {
        BinaryReader reader(data);
        const auto type = static_cast<PacketType>(reader.ReadUInt8());
        if (!reader.Ok())
        {
            return;
        }

        if (isHost)
        {
            if (type == PacketType::Hello)
            {
                HandleHello(sender, reader);
                return;
            }
            // Everything else is only accepted from admitted players.
            if (FindPlayer(sender) == nullptr)
            {
                return;
            }
            HandleHostPacket(sender, type, reader);
        }
        else
        {
            // A client only ever talks to its host.
            if (sender != host)
            {
                return;
            }
            HandleClientPacket(sender, type, reader);
        }
    }

    void HandleHostPacket(PlayerID sender, PacketType type, BinaryReader& reader)
    {
        switch (type)
        {
        case PacketType::SetReady:
        {
            const bool ready = reader.ReadBool();
            PlayerInfo* player = FindPlayer(sender);
            if (reader.Ok() && state == SessionState::InLobby && player != nullptr)
            {
                player->ready = ready;
                BroadcastLobbyState();
                NotifyPlayers();
            }
            break;
        }

        case PacketType::GameLoaded:
            if (state == SessionState::PreparingGame)
            {
                loaded[sender] = true;
                TryStartGame();
            }
            else if (state == SessionState::InGame)
            {
                // A player that joined in progress finished loading.
                loaded[sender] = true;
                BinaryWriter writer;
                writer.Write(static_cast<std::uint8_t>(PacketType::StartGame));
                SendPacket(sender, writer);
            }
            break;

        case PacketType::SessionEnd:
            RemovePlayer(sender, false);
            break;

        case PacketType::UserMessage:
            net.messages_.Dispatch(sender, false, reader);
            break;

        default:
            net.replication_.HandlePacket(sender, type, reader);
            break;
        }
    }

    void HandleClientPacket(PlayerID sender, PacketType type, BinaryReader& reader)
    {
        switch (type)
        {
        case PacketType::Welcome:
        {
            if (state != SessionState::Connecting)
            {
                break;
            }
            const PlayerID announcedHost = reader.ReadUInt64();
            std::vector<PlayerInfo> list;
            if (announcedHost != host || !ReadPlayers(reader, list))
            {
                Fail(SessionEndReason::NetworkError, "Malformed welcome from the host.");
                break;
            }
            players = std::move(list);
            net.replication_.StartClient(local, host, MakeSender());
            SetState(SessionState::InLobby);
            for (const PlayerInfo& player : players)
            {
                if (onPlayerJoined && player.id != local)
                {
                    onPlayerJoined(player);
                }
            }
            NotifyPlayers();
            break;
        }

        case PacketType::Reject:
        {
            const auto reason = static_cast<RejectReason>(reader.ReadUInt8());
            switch (reason)
            {
            case RejectReason::ProtocolMismatch:
            case RejectReason::GameVersionMismatch:
            case RejectReason::SchemaMismatch:
                Fail(SessionEndReason::ProtocolMismatch, "The host runs an incompatible version of the game.");
                break;
            case RejectReason::SessionFull:
                Fail(SessionEndReason::NetworkError, "The session is full.");
                break;
            default:
                Fail(SessionEndReason::NetworkError, "The host refused the connection (game already in progress).");
                break;
            }
            break;
        }

        case PacketType::LobbyState:
        {
            std::vector<PlayerInfo> list;
            if (!ReadPlayers(reader, list))
            {
                break;
            }
            std::unordered_set<PlayerID> previous;
            for (const PlayerInfo& player : players)
            {
                previous.insert(player.id);
            }
            std::unordered_set<PlayerID> current;
            for (const PlayerInfo& player : list)
            {
                current.insert(player.id);
            }
            const std::vector<PlayerInfo> before = std::move(players);
            players = std::move(list);

            for (const PlayerInfo& player : players)
            {
                if (!previous.contains(player.id) && player.id != local && onPlayerJoined)
                {
                    onPlayerJoined(player);
                }
            }
            for (const PlayerInfo& player : before)
            {
                if (!current.contains(player.id) && onPlayerLeft)
                {
                    onPlayerLeft(player.id);
                }
            }
            NotifyPlayers();
            break;
        }

        case PacketType::PrepareGame:
        {
            GameStartInfo info;
            info.map = reader.ReadString(1024);
            info.gameMode = reader.ReadString(256);
            info.seed = reader.ReadUInt64();
            const std::size_t size = reader.ReadUInt32();
            if (size > reader.Remaining())
            {
                break;
            }
            info.customData.resize(size);
            if (!reader.ReadBytes(info.customData.data(), size) || state != SessionState::InLobby)
            {
                break;
            }
            startInfo = info;
            SetState(SessionState::PreparingGame);
            if (onPrepareGame)
            {
                onPrepareGame(startInfo);
            }
            break;
        }

        case PacketType::StartGame:
            if (state == SessionState::PreparingGame)
            {
                SetState(SessionState::InGame);
                if (onGameStart)
                {
                    onGameStart();
                }
            }
            break;

        case PacketType::SessionEnd:
        {
            const auto reason = static_cast<SessionEndReason>(reader.ReadUInt8());
            Teardown(reader.Ok() ? reason : SessionEndReason::HostEndedGame, true, SessionState::Offline);
            break;
        }

        case PacketType::Kick:
            Teardown(SessionEndReason::Kicked, true, SessionState::Offline);
            break;

        case PacketType::UserMessage:
            if (state != SessionState::Connecting)
            {
                net.messages_.Dispatch(sender, true, reader);
            }
            break;

        default:
            if (state != SessionState::Connecting)
            {
                net.replication_.HandlePacket(sender, type, reader);
            }
            break;
        }
    }

    // ---- Game flow ----------------------------------------------------------------------------------------

    void SendPrepareGame(PlayerID to)
    {
        BinaryWriter writer;
        writer.Write(static_cast<std::uint8_t>(PacketType::PrepareGame));
        writer.Write(startInfo.map);
        writer.Write(startInfo.gameMode);
        writer.Write(startInfo.seed);
        writer.Write(static_cast<std::uint32_t>(startInfo.customData.size()));
        writer.WriteBytes(startInfo.customData);
        SendPacket(to, writer);
    }

    bool AllReady() const
    {
        return isHost && !players.empty() &&
               std::all_of(players.begin(), players.end(), [](const PlayerInfo& p) { return p.ready; });
    }

    void SetReady(bool ready)
    {
        if (!initialized || state != SessionState::InLobby)
        {
            return;
        }
        if (isHost)
        {
            if (PlayerInfo* self = FindPlayer(local))
            {
                self->ready = ready;
            }
            BroadcastLobbyState();
            NotifyPlayers();
        }
        else
        {
            BinaryWriter writer;
            writer.Write(static_cast<std::uint8_t>(PacketType::SetReady));
            writer.Write(ready);
            SendPacket(host, writer);
        }
    }

    bool PrepareGame(const GameStartInfo& info)
    {
        if (!isHost || state != SessionState::InLobby || !AllReady())
        {
            return false;
        }
        startInfo = info;
        for (const PlayerInfo& player : players)
        {
            loaded[player.id] = false;
        }
        if (!config.allowJoinInProgress)
        {
            lobby->SetJoinable(false);
        }
        SetState(SessionState::PreparingGame);
        for (const PlayerInfo& player : players)
        {
            if (player.id != local)
            {
                SendPrepareGame(player.id);
            }
        }
        if (onPrepareGame)
        {
            onPrepareGame(startInfo);
        }
        return true;
    }

    void ConfirmGameLoaded()
    {
        if (!initialized || state != SessionState::PreparingGame)
        {
            return;
        }
        if (isHost)
        {
            loaded[local] = true;
            TryStartGame();
        }
        else
        {
            BinaryWriter writer;
            writer.Write(static_cast<std::uint8_t>(PacketType::GameLoaded));
            SendPacket(host, writer);
        }
    }

    void TryStartGame()
    {
        if (!isHost || state != SessionState::PreparingGame)
        {
            return;
        }
        const bool everyoneLoaded = std::all_of(players.begin(), players.end(),
                                                [this](const PlayerInfo& p)
                                                {
                                                    const auto found = loaded.find(p.id);
                                                    return found != loaded.end() && found->second;
                                                });
        if (!everyoneLoaded)
        {
            return;
        }

        SetState(SessionState::InGame);
        BinaryWriter writer;
        writer.Write(static_cast<std::uint8_t>(PacketType::StartGame));
        SendToClients(writer);
        if (onGameStart)
        {
            onGameStart();
        }
    }

    void Kick(PlayerID player)
    {
        if (!isHost || player == local || FindPlayer(player) == nullptr)
        {
            return;
        }
        BinaryWriter writer;
        writer.Write(static_cast<std::uint8_t>(PacketType::Kick));
        SendPacket(player, writer);
        RemovePlayer(player, true);
    }

    void Leave()
    {
        if (!initialized || (Idle() && !isHost))
        {
            return;
        }
        if (isHost)
        {
            BinaryWriter writer;
            writer.Write(static_cast<std::uint8_t>(PacketType::SessionEnd));
            writer.Write(static_cast<std::uint8_t>(SessionEndReason::HostEndedGame));
            SendToClients(writer);
        }
        else if (state == SessionState::InLobby || state == SessionState::PreparingGame || state == SessionState::InGame)
        {
            BinaryWriter writer;
            writer.Write(static_cast<std::uint8_t>(PacketType::SessionEnd));
            writer.Write(static_cast<std::uint8_t>(SessionEndReason::ConnectionLost));
            SendPacket(host, writer);
        }
        Teardown(SessionEndReason::HostEndedGame, false, SessionState::Offline);
    }

    struct PendingHello
    {
        Hello hello;
        Clock::time_point deadline;
    };

    Network& net;
    NetworkConfig config;

    bool initialized = false;
    bool ownsSteam = false;
    std::string lastError;

    std::unique_ptr<SteamTransport> transport;
    std::unique_ptr<LobbyManager> lobby;
    std::vector<SteamTransport::Event> events;

    SessionState state = SessionState::Offline;
    PlayerID local = InvalidPlayer;
    PlayerID host = InvalidPlayer;
    bool isHost = false;
    std::vector<PlayerInfo> players;
    std::unordered_map<PlayerID, bool> loaded;
    GameStartInfo startInfo;
    std::vector<LobbyInfo> lobbyResults;

    std::unordered_map<PlayerID, Clock::time_point> unidentified;
    std::unordered_map<PlayerID, PendingHello> awaitingMembership;
    Clock::time_point connectDeadline;

    std::function<void(SessionEndReason)> onSessionEnded;
    std::function<void(SessionState)> onStateChanged;
    std::function<void(const std::vector<LobbyInfo>&)> onLobbyList;
    std::function<void(const std::vector<PlayerInfo>&)> onPlayersChanged;
    std::function<void(const PlayerInfo&)> onPlayerJoined;
    std::function<void(PlayerID)> onPlayerLeft;
    std::function<void(const GameStartInfo&)> onPrepareGame;
    std::function<void()> onGameStart;
};

// ---- Network ------------------------------------------------------------------------------------------------

Network::Network(entt::registry& registry, NetworkConfig config)
    : config_(std::move(config)), replication_(registry, config_.replication)
{
    impl_ = std::make_unique<Impl>(*this);
}

Network::~Network()
{
    Shutdown();
}

bool Network::Initialize() { return impl_->Initialize(); }
void Network::Shutdown() { impl_->Shutdown(); }
bool Network::IsInitialized() const noexcept { return impl_->initialized; }
const std::string& Network::LastError() const noexcept { return impl_->lastError; }

void Network::Poll() { impl_->Poll(); }
void Network::Flush(float deltaTime) { replication_.Flush(deltaTime); }

void Network::Update(float deltaTime)
{
    Poll();
    Flush(deltaTime);
}

bool Network::FindPublicLobbies(const LobbyFilter& filter) { return impl_->FindPublicLobbies(filter); }

bool Network::CreateLobby(LobbyVisibility visibility, const LobbyOptions& options)
{
    return impl_->CreateLobby(visibility, options);
}

bool Network::JoinLobby(std::uint64_t lobbyId) { return impl_->JoinLobby(lobbyId); }

bool Network::JoinLobbyFromCommandLine(int argc, const char* const* argv)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "+connect_lobby") == 0)
        {
            const std::uint64_t id = std::strtoull(argv[i + 1], nullptr, 10);
            return id != 0 && JoinLobby(id);
        }
    }
    return false;
}

void Network::OpenInviteOverlay()
{
    if (impl_->initialized && impl_->lobby)
    {
        impl_->lobby->OpenInviteOverlay();
    }
}

const std::vector<LobbyInfo>& Network::LobbyResults() const noexcept { return impl_->lobbyResults; }
std::uint64_t Network::LobbyId() const noexcept { return impl_->lobby ? impl_->lobby->Current() : 0; }

SessionState Network::State() const noexcept { return impl_->state; }
bool Network::IsHost() const noexcept { return impl_->isHost; }

bool Network::InSession() const noexcept
{
    const SessionState state = impl_->state;
    return state == SessionState::InLobby || state == SessionState::PreparingGame || state == SessionState::InGame;
}

PlayerID Network::LocalPlayer() const noexcept { return impl_->local; }
PlayerID Network::HostPlayer() const noexcept { return impl_->host; }
const std::vector<PlayerInfo>& Network::Players() const noexcept { return impl_->players; }

void Network::SetReady(bool ready) { impl_->SetReady(ready); }
bool Network::AllPlayersReady() const { return impl_->AllReady(); }
bool Network::PrepareGame(const GameStartInfo& info) { return impl_->PrepareGame(info); }
void Network::ConfirmGameLoaded() { impl_->ConfirmGameLoaded(); }
void Network::KickPlayer(PlayerID player) { impl_->Kick(player); }
void Network::LeaveSession() { impl_->Leave(); }

void Network::OnSessionEnded(std::function<void(SessionEndReason)> callback) { impl_->onSessionEnded = std::move(callback); }
void Network::OnStateChanged(std::function<void(SessionState)> callback) { impl_->onStateChanged = std::move(callback); }
void Network::OnLobbyList(std::function<void(const std::vector<LobbyInfo>&)> callback) { impl_->onLobbyList = std::move(callback); }
void Network::OnPlayersChanged(std::function<void(const std::vector<PlayerInfo>&)> callback) { impl_->onPlayersChanged = std::move(callback); }
void Network::OnPlayerJoined(std::function<void(const PlayerInfo&)> callback) { impl_->onPlayerJoined = std::move(callback); }
void Network::OnPlayerLeft(std::function<void(PlayerID)> callback) { impl_->onPlayerLeft = std::move(callback); }
void Network::OnPrepareGame(std::function<void(const GameStartInfo&)> callback) { impl_->onPrepareGame = std::move(callback); }
void Network::OnGameStart(std::function<void()> callback) { impl_->onGameStart = std::move(callback); }

bool Network::SendToHostBytes(std::span<const std::uint8_t> bytes, Delivery delivery)
{
    if (!InSession())
    {
        return false;
    }
    if (impl_->isHost)
    {
        // Loopback so that game code can send "to the host" without caring whether it is the host.
        BinaryReader reader(bytes.subspan(1));
        return messages_.Dispatch(impl_->local, false, reader);
    }
    return impl_->SendBytes(impl_->host, bytes, delivery);
}

bool Network::BroadcastBytes(std::span<const std::uint8_t> bytes, Delivery delivery)
{
    if (!InSession() || !impl_->isHost)
    {
        return false;
    }
    impl_->SendToClients(bytes, delivery);
    return true;
}

bool Network::SendToBytes(PlayerID player, std::span<const std::uint8_t> bytes, Delivery delivery)
{
    if (!InSession() || !impl_->isHost || player == impl_->local || impl_->FindPlayer(player) == nullptr)
    {
        return false;
    }
    return impl_->SendBytes(player, bytes, delivery);
}

} // namespace steamnet
