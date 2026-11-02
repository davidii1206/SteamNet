// Two-player example: every player owns a Transform that the host moves from the player's input and
// replicates to everyone.
//
//   steamnet_example host             create a friends-only lobby and wait for a friend
//   steamnet_example browse           list public lobbies
//   steamnet_example join <lobbyId>   join a lobby (also: +connect_lobby <lobbyId>)
//
// Requires a running Steam client and a steam_appid.txt next to the executable (480 = Spacewar for testing).

#include "steamnet/Network.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace
{

struct Transform
{
    float x = 0;
    float y = 0;
};

struct Health
{
    float current = 100;
    float maximum = 100;
};

struct PlayerInput
{
    std::int8_t moveX = 0;
    std::int8_t moveY = 0;
};

} // namespace

int main(int argc, char** argv)
{
    entt::registry registry;
    steamnet::NetworkConfig config;
    config.gameVersion = "example-1";
    config.maxPlayers = 2;
    steamnet::Network network(registry, config);

    if (!network.Initialize())
    {
        std::printf("Steam initialization failed: %s\n", network.LastError().c_str());
        return 1;
    }

    // Host and clients register the same components and messages in the same order.
    network.Replication().Register<Transform>({.authority = steamnet::Authority::Host,
                                               .audience = steamnet::Audience::Everyone,
                                               .delivery = steamnet::Delivery::Unreliable,
                                               .updateMode = steamnet::UpdateMode::Periodic});
    network.Replication().Register<Health>({.authority = steamnet::Authority::Host,
                                            .audience = steamnet::Audience::Everyone,
                                            .delivery = steamnet::Delivery::Reliable,
                                            .updateMode = steamnet::UpdateMode::OnChange});

    std::unordered_map<steamnet::PlayerID, PlayerInput> inputs;
    network.Messages().Register<PlayerInput>(
        [&](steamnet::PlayerID sender, const PlayerInput& input) { inputs[sender] = input; },
        {.direction = steamnet::MessageDirection::ClientToHost});

    std::unordered_map<steamnet::PlayerID, entt::entity> avatars;

    network.OnStateChanged([](steamnet::SessionState state) { std::printf("state -> %d\n", static_cast<int>(state)); });
    network.OnSessionEnded([](steamnet::SessionEndReason reason)
                           { std::printf("session ended (reason %d): back to the menu\n", static_cast<int>(reason)); });
    network.OnLobbyList(
        [](const std::vector<steamnet::LobbyInfo>& lobbies)
        {
            for (const auto& lobby : lobbies)
            {
                std::printf("lobby %llu \"%s\" %d/%d%s\n", static_cast<unsigned long long>(lobby.id), lobby.name.c_str(),
                            lobby.players, lobby.maxPlayers, lobby.joinable ? "" : " (not joinable)");
            }
        });
    network.OnPlayerJoined(
        [&](const steamnet::PlayerInfo& player)
        {
            std::printf("%s joined\n", player.name.c_str());
            if (network.IsHost())
            {
                avatars[player.id] = network.Spawn(player.id);
                registry.emplace<Transform>(avatars[player.id]);
                registry.emplace<Health>(avatars[player.id]);
            }
        });
    network.OnPlayerLeft([&](steamnet::PlayerID player)
                         {
                             std::printf("player %llu left\n", static_cast<unsigned long long>(player));
                             avatars.erase(player); // the host destroys entities owned by a leaving player
                         });
    network.OnPrepareGame(
        [&](const steamnet::GameStartInfo& info)
        {
            std::printf("loading map '%s'...\n", info.map.c_str());
            network.ConfirmGameLoaded(); // a real game would confirm after loading its assets
        });
    network.OnGameStart([] { std::printf("game started\n"); });

    const char* mode = argc > 1 ? argv[1] : "host";
    if (network.JoinLobbyFromCommandLine(argc, argv))
    {
    }
    else if (std::strcmp(mode, "host") == 0)
    {
        network.CreateLobby(steamnet::LobbyVisibility::FriendsOnly, {.name = "Example lobby", .gameMode = "coop"});
    }
    else if (std::strcmp(mode, "browse") == 0)
    {
        network.FindPublicLobbies();
    }
    else if (std::strcmp(mode, "join") == 0 && argc > 2)
    {
        network.JoinLobby(std::strtoull(argv[2], nullptr, 10));
    }

    bool readySent = false;
    bool prepared = false;
    float sendInput = 0;
    float log = 0;
    constexpr float dt = 1.0f / 60.0f;

    for (int frame = 0; frame < 60 * 120; ++frame)
    {
        network.Poll();

        // ---- Game simulation ----
        if (network.State() == steamnet::SessionState::InLobby)
        {
            if (!readySent && network.Players().size() == 2)
            {
                network.SetReady(true);
                readySent = true;
            }
            if (network.IsHost() && !prepared && network.AllPlayersReady())
            {
                prepared = network.PrepareGame({.map = "arena"});
            }
        }
        if (network.State() == steamnet::SessionState::InGame)
        {
            if (network.IsHost())
            {
                for (auto& [player, entity] : avatars)
                {
                    const PlayerInput input = inputs.contains(player) ? inputs[player] : PlayerInput{};
                    auto& transform = registry.get<Transform>(entity);
                    transform.x += static_cast<float>(input.moveX) * dt;
                    transform.y += static_cast<float>(input.moveY) * dt;
                }
                // The host controls its own avatar directly.
                if (avatars.contains(network.LocalPlayer()))
                {
                    registry.get<Transform>(avatars[network.LocalPlayer()]).x += dt;
                }
            }
            else if ((sendInput += dt) > 0.1f)
            {
                sendInput = 0;
                network.SendToHost(PlayerInput{1, 0});
            }

            if ((log += dt) > 1.0f)
            {
                log = 0;
                for (auto [entity, transform] : registry.view<Transform>().each())
                {
                    const auto owner = registry.get<steamnet::NetworkOwner>(entity).owner;
                    std::printf("entity of %llu at (%.2f, %.2f)\n", static_cast<unsigned long long>(owner), transform.x,
                                transform.y);
                }
            }
        }

        network.Flush(dt);
        std::this_thread::sleep_for(std::chrono::duration<float>(dt));
    }

    network.LeaveSession();
    return 0;
}
