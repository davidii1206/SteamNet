// Tests for the transport-independent parts of steamnet. Host and clients are connected through in-memory
// queues, so no Steam client is needed.

#include "steamnet/Messages/MessageRegistry.hpp"
#include "steamnet/Replication/Replication.hpp"
#include "steamnet/Serialization/Serializer.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <string>

using namespace steamnet;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        ++g_checks;                                                                                \
        if (!(condition))                                                                          \
        {                                                                                          \
            ++g_failures;                                                                          \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                     \
        }                                                                                          \
    } while (false)

struct Transform
{
    float x = 0;
    float y = 0;
    float z = 0;
};

struct Health
{
    float current = 100;
    float maximum = 100;
};

struct Inventory
{
    std::vector<std::int32_t> items;
};

struct InputState
{
    std::int8_t moveX = 0;
    bool fire = false;
};

struct Tag
{
};

enum class Team : std::uint8_t { Red, Blue };

struct Profile
{
    std::string name;
    Team team = Team::Red;
    std::optional<std::int32_t> score;
    std::map<std::string, std::int32_t> stats;
    std::array<std::uint8_t, 3> color{};
    std::vector<Transform> trail;
};

constexpr PlayerID HostId = 1000;
constexpr PlayerID AliceId = 2001;
constexpr PlayerID BobId = 2002;

// A Replication instance bound to its own registry, plus its inbox.
struct Peer
{
    entt::registry registry;
    Replication replication{registry};
    std::deque<std::pair<PlayerID, std::vector<std::uint8_t>>> inbox; // (sender, bytes)
    int unreliableSent = 0;
    int reliableSent = 0;
};

// Wires a host and any number of clients together.
struct World
{
    Peer host;
    std::map<PlayerID, std::unique_ptr<Peer>> clients;
    bool dropUnreliable = false;

    World()
    {
        host.replication.StartHost(HostId, MakeSender(host, HostId, true));
    }

    Replication::SendFunction MakeSender(Peer& from, PlayerID fromId, bool isHost)
    {
        return [this, &from, fromId, isHost](PlayerID to, std::span<const std::uint8_t> data, Delivery delivery)
        {
            if (delivery == Delivery::Unreliable)
            {
                ++from.unreliableSent;
                if (dropUnreliable)
                {
                    return;
                }
            }
            else
            {
                ++from.reliableSent;
            }
            Peer* target = nullptr;
            if (isHost)
            {
                target = clients.at(to).get();
            }
            else
            {
                target = &host;
            }
            target->inbox.emplace_back(fromId, std::vector<std::uint8_t>(data.begin(), data.end()));
        };
    }

    template<typename Setup>
    Peer& AddClient(PlayerID id, Setup&& setup)
    {
        auto peer = std::make_unique<Peer>();
        Peer& ref = *peer;
        setup(ref.replication);
        clients.emplace(id, std::move(peer));
        ref.replication.StartClient(id, HostId, MakeSender(ref, id, false));
        host.replication.AddPeer(id);
        Pump();
        return ref;
    }

    void Deliver(Peer& peer)
    {
        while (!peer.inbox.empty())
        {
            auto [sender, bytes] = std::move(peer.inbox.front());
            peer.inbox.pop_front();
            BinaryReader reader(bytes);
            const auto type = static_cast<PacketType>(reader.ReadUInt8());
            peer.replication.HandlePacket(sender, type, reader);
        }
    }

    // One network tick on every peer, then deliver everything (including replies) until quiet.
    void Pump(float dt = 0.1f)
    {
        host.replication.Flush(dt);
        for (auto& [id, client] : clients)
        {
            client->replication.Flush(dt);
        }
        for (int round = 0; round < 4; ++round)
        {
            Deliver(host);
            for (auto& [id, client] : clients)
            {
                Deliver(*client);
            }
        }
    }
};

template<typename Setup>
void RegisterCommon(Replication& replication, Setup&& extra)
{
    replication.Register<Transform>({.authority = Authority::Host,
                                     .audience = Audience::Everyone,
                                     .delivery = Delivery::Unreliable,
                                     .updateMode = UpdateMode::Periodic});
    replication.Register<Health>({.authority = Authority::Host,
                                  .audience = Audience::Everyone,
                                  .delivery = Delivery::Reliable,
                                  .updateMode = UpdateMode::OnChange});
    replication.Register<Inventory>({.authority = Authority::Host,
                                     .audience = Audience::OwnerOnly,
                                     .delivery = Delivery::Reliable,
                                     .updateMode = UpdateMode::OnChange});
    replication.Register<Tag>({});
    extra(replication);
}

void RegisterAll(Replication& replication)
{
    RegisterCommon(replication,
                   [](Replication& r)
                   {
                       r.Register<InputState>({.authority = Authority::OwningClient,
                                               .audience = Audience::HostOnly,
                                               .delivery = Delivery::Reliable,
                                               .updateMode = UpdateMode::OnChange});
                   });
}

entt::entity Mirror(Peer& peer, NetEntityID id)
{
    return peer.replication.Find(id);
}

// ---- Tests --------------------------------------------------------------------------------------------------

void TestSerialization()
{
    Profile profile;
    profile.name = "alice";
    profile.team = Team::Blue;
    profile.score = 42;
    profile.stats = {{"kills", 3}, {"deaths", 1}};
    profile.color = {1, 2, 3};
    profile.trail = {{1, 2, 3}, {4, 5, 6}};

    BinaryWriter writer;
    Serializer<Profile>::Write(writer, profile);

    BinaryReader reader(writer.Bytes());
    const Profile copy = Serializer<Profile>::Read(reader);
    CHECK(reader.Ok());
    CHECK(reader.AtEnd());
    CHECK(copy.name == "alice");
    CHECK(copy.team == Team::Blue);
    CHECK(copy.score == 42);
    CHECK(copy.stats.at("kills") == 3);
    CHECK(copy.color[2] == 3);
    CHECK(copy.trail.size() == 2 && copy.trail[1].z == 6);

    // Fixed little-endian layout, independent of the host.
    BinaryWriter ints;
    ints.Write(std::uint32_t{0x01020304});
    CHECK(ints.Bytes()[0] == 0x04 && ints.Bytes()[3] == 0x01);
}

void TestReaderRejectsMalformedInput()
{
    BinaryWriter writer;
    Profile profile;
    profile.name = "x";
    Serializer<Profile>::Write(writer, profile);

    // Every truncation must fail cleanly rather than read out of bounds.
    for (std::size_t length = 0; length < writer.Size(); ++length)
    {
        BinaryReader reader(writer.Bytes().data(), length);
        (void)Serializer<Profile>::Read(reader);
        CHECK(!reader.Ok());
    }

    // A hostile length prefix must not allocate gigabytes.
    BinaryWriter hostile;
    hostile.Write(std::uint32_t{0xFFFFFFFFu});
    BinaryReader reader(hostile.Bytes());
    const auto items = Serializer<std::vector<std::int32_t>>::Read(reader);
    CHECK(!reader.Ok());
    CHECK(items.empty());

    BinaryReader stringReader(hostile.Bytes());
    CHECK(stringReader.ReadString().empty());
    CHECK(!stringReader.Ok());
}

void TestSpawnAndReplicate()
{
    World world;
    RegisterAll(world.host.replication);
    Peer& alice = world.AddClient(AliceId, RegisterAll);

    const entt::entity entity = world.host.replication.Spawn(AliceId);
    CHECK(entity != entt::null);
    world.host.registry.emplace<Transform>(entity, Transform{1, 2, 3});
    world.host.registry.emplace<Health>(entity, Health{80, 100});
    world.Pump();

    const NetEntityID id = world.host.replication.GetNetworkID(entity);
    CHECK(id != InvalidNetEntity);
    const entt::entity mirror = Mirror(alice, id);
    CHECK(mirror != entt::null);
    CHECK(alice.registry.get<Transform>(mirror).y == 2);
    CHECK(alice.registry.get<Health>(mirror).current == 80);
    CHECK(alice.registry.get<NetworkOwner>(mirror).owner == AliceId);
    CHECK(alice.replication.IsOwnedLocally(mirror));

    // registry.patch triggers an OnChange update.
    world.host.registry.patch<Health>(entity, [](Health& health) { health.current -= 10; });
    world.Pump();
    CHECK(alice.registry.get<Health>(mirror).current == 70);

    // Edit<T> is the explicit alternative for code that mutates through references.
    world.host.replication.Edit<Health>(entity).current = 55;
    world.Pump();
    CHECK(alice.registry.get<Health>(mirror).current == 55);

    // A raw mutation without patch/Edit is not detected for OnChange components (documented limitation)...
    world.host.registry.get<Health>(entity).current = 1;
    world.Pump();
    CHECK(alice.registry.get<Health>(mirror).current == 55);
    // ...but is picked up by polled components.
    world.host.registry.get<Transform>(entity).x = 9;
    world.Pump();
    CHECK(alice.registry.get<Transform>(mirror).x == 9);

    // Empty tag components replicate their presence.
    world.host.registry.emplace<Tag>(entity);
    world.Pump();
    CHECK(alice.registry.all_of<Tag>(mirror));
    world.host.registry.remove<Tag>(entity);
    world.Pump();
    CHECK(!alice.registry.all_of<Tag>(mirror));

    world.host.registry.remove<Health>(entity);
    world.Pump();
    CHECK(!alice.registry.all_of<Health>(mirror));

    world.host.replication.Destroy(entity);
    world.Pump();
    CHECK(alice.replication.NetworkEntityCount() == 0);
    CHECK(alice.registry.view<NetworkEntity>().size() == 0);
}

void TestLateJoinSnapshot()
{
    World world;
    RegisterAll(world.host.replication);

    const entt::entity first = world.host.replication.Spawn();
    world.host.registry.emplace<Transform>(first, Transform{5, 6, 7});
    const entt::entity second = world.host.replication.Spawn(AliceId);
    world.host.registry.emplace<Inventory>(second, Inventory{{1, 2, 3}});
    world.host.registry.emplace<Health>(second);
    world.Pump();

    bool synchronized = false;
    bool peerSynchronized = false;
    world.host.replication.OnPeerSynchronized([&](PlayerID peer) { peerSynchronized = peer == BobId; });
    Peer& bob = world.AddClient(BobId, [&](Replication& r)
                                {
                                    RegisterAll(r);
                                    r.OnSynchronized([&] { synchronized = true; });
                                });
    CHECK(synchronized);
    CHECK(peerSynchronized);
    CHECK(bob.replication.NetworkEntityCount() == 2);

    const entt::entity mirrorFirst = Mirror(bob, world.host.replication.GetNetworkID(first));
    const entt::entity mirrorSecond = Mirror(bob, world.host.replication.GetNetworkID(second));
    CHECK(bob.registry.get<Transform>(mirrorFirst).z == 7);
    CHECK(bob.registry.all_of<Health>(mirrorSecond));
    // Alice's inventory is OwnerOnly: Bob must not see it.
    CHECK(!bob.registry.all_of<Inventory>(mirrorSecond));
    CHECK(bob.registry.get<NetworkOwner>(mirrorSecond).owner == AliceId);
}

void TestAudiences()
{
    World world;
    RegisterAll(world.host.replication);
    Peer& alice = world.AddClient(AliceId, RegisterAll);
    Peer& bob = world.AddClient(BobId, RegisterAll);

    const entt::entity entity = world.host.replication.Spawn(AliceId);
    world.host.registry.emplace<Inventory>(entity, Inventory{{7}});
    world.host.registry.emplace<Health>(entity);
    world.Pump();

    const NetEntityID id = world.host.replication.GetNetworkID(entity);
    CHECK(alice.registry.all_of<Inventory>(Mirror(alice, id)));
    CHECK(!bob.registry.all_of<Inventory>(Mirror(bob, id)));
    CHECK(bob.registry.all_of<Health>(Mirror(bob, id)));

    world.host.registry.patch<Inventory>(entity, [](Inventory& inventory) { inventory.items.push_back(8); });
    world.Pump();
    CHECK(alice.registry.get<Inventory>(Mirror(alice, id)).items.size() == 2);
    CHECK(!bob.registry.all_of<Inventory>(Mirror(bob, id)));

    // Ownership transfer moves OwnerOnly data.
    world.host.replication.SetOwner(entity, BobId);
    world.Pump();
    CHECK(!alice.registry.all_of<Inventory>(Mirror(alice, id)));
    CHECK(bob.registry.get<Inventory>(Mirror(bob, id)).items.size() == 2);
    CHECK(bob.replication.IsOwnedLocally(Mirror(bob, id)));
}

void TestPeriodicSkipsUnchangedAndRefreshes()
{
    World world;
    RegisterAll(world.host.replication);
    Peer& alice = world.AddClient(AliceId, RegisterAll);

    const entt::entity entity = world.host.replication.Spawn();
    world.host.registry.emplace<Transform>(entity, Transform{1, 1, 1});
    world.Pump();
    const int afterSpawn = world.host.unreliableSent;

    for (int i = 0; i < 5; ++i)
    {
        world.Pump(0.1f); // 0.5s total, below the 1s refresh interval
    }
    CHECK(world.host.unreliableSent == afterSpawn);

    for (int i = 0; i < 8; ++i)
    {
        world.Pump(0.2f);
    }
    CHECK(world.host.unreliableSent > afterSpawn); // refreshed

    // A lost unreliable update is healed by the next change/refresh.
    const NetEntityID id = world.host.replication.GetNetworkID(entity);
    world.dropUnreliable = true;
    world.host.registry.get<Transform>(entity).x = 50;
    world.Pump();
    CHECK(alice.registry.get<Transform>(Mirror(alice, id)).x == 1);
    world.dropUnreliable = false;
    for (int i = 0; i < 8; ++i)
    {
        world.Pump(0.2f);
    }
    CHECK(alice.registry.get<Transform>(Mirror(alice, id)).x == 50);
}

void TestStaleUnreliableUpdatesAreDiscarded()
{
    World world;
    RegisterAll(world.host.replication);
    Peer& alice = world.AddClient(AliceId, RegisterAll);

    const entt::entity entity = world.host.replication.Spawn();
    world.host.registry.emplace<Transform>(entity, Transform{1, 0, 0});
    world.Pump();
    const NetEntityID id = world.host.replication.GetNetworkID(entity);

    world.host.registry.get<Transform>(entity).x = 2;
    world.host.replication.Flush(0.1f);
    world.host.registry.get<Transform>(entity).x = 3;
    world.host.replication.Flush(0.1f);

    // Deliver the newest packet first, then the older one.
    CHECK(alice.inbox.size() == 2);
    std::swap(alice.inbox.front(), alice.inbox.back());
    world.Deliver(alice);
    CHECK(alice.registry.get<Transform>(Mirror(alice, id)).x == 3);
}

void TestClientAuthority()
{
    World world;

    int validated = 0;
    RegisterCommon(world.host.replication,
                   [&](Replication& r)
                   {
                       r.Register<InputState>(
                           {.authority = Authority::OwningClient, .audience = Audience::HostOnly},
                           [&](PlayerID, entt::entity, const InputState& input)
                           {
                               ++validated;
                               return input.moveX >= -1 && input.moveX <= 1;
                           });
                   });

    Peer& alice = world.AddClient(AliceId, RegisterAll);
    Peer& bob = world.AddClient(BobId, RegisterAll);

    const entt::entity entity = world.host.replication.Spawn(AliceId);
    world.host.registry.emplace<InputState>(entity);
    world.Pump();
    const NetEntityID id = world.host.replication.GetNetworkID(entity);

    // The owner may author InputState.
    // HostOnly components are not replicated to clients, so the owner creates its own copy.
    alice.registry.emplace<InputState>(Mirror(alice, id), InputState{1, true});
    world.Pump();
    CHECK(world.host.registry.get<InputState>(entity).moveX == 1);
    CHECK(world.host.registry.get<InputState>(entity).fire);
    CHECK(validated == 1);

    // HostOnly: never replicated to other clients.
    CHECK(!bob.registry.all_of<InputState>(Mirror(bob, id)));

    // The validator can reject, and the client is corrected back to the authoritative value.
    alice.registry.patch<InputState>(Mirror(alice, id), [](InputState& input) { input.moveX = 100; });
    world.Pump();
    CHECK(validated == 2);
    CHECK(world.host.registry.get<InputState>(entity).moveX == 1);
    world.Pump(); // the correction is sent on the host's next tick
    CHECK(alice.registry.get<InputState>(Mirror(alice, id)).moveX == 1);

    // Bob does not own the entity, so his update is dropped before the validator runs.
    Peer& impostor = bob;
    impostor.registry.emplace<InputState>(Mirror(impostor, id));
    world.Pump();
    CHECK(world.host.registry.get<InputState>(entity).moveX == 1);
    CHECK(validated == 2);

    // Components with Authority::Host cannot be authored by clients, even by the owner.
    BinaryWriter forged;
    forged.Write(static_cast<std::uint8_t>(PacketType::ReplicationBatch));
    forged.Write(static_cast<std::uint8_t>(ReplicationOp::UpdateComponent));
    forged.Write(id);
    forged.Write(ComponentTypeID{1}); // Health
    forged.Write(std::uint32_t{99});
    BinaryWriter payload;
    Serializer<Health>::Write(payload, Health{1, 1});
    forged.Write(static_cast<std::uint32_t>(payload.Size()));
    forged.WriteBytes(payload.Bytes());
    world.host.registry.emplace<Health>(entity);
    world.host.inbox.emplace_back(AliceId, forged.Buffer());
    world.Pump();
    CHECK(world.host.registry.get<Health>(entity).current == 100);

    // Garbage from a client must not crash the host.
    world.host.inbox.emplace_back(AliceId, std::vector<std::uint8_t>{static_cast<std::uint8_t>(PacketType::ReplicationBatch), 3, 1, 2});
    world.host.inbox.emplace_back(BobId, std::vector<std::uint8_t>{static_cast<std::uint8_t>(PacketType::ReplicationBatch), 0xFF});
    world.Pump();
}

void TestClientsCannotInjectOtherOps()
{
    World world;
    RegisterAll(world.host.replication);
    Peer& alice = world.AddClient(AliceId, RegisterAll);
    (void)alice;

    const entt::entity entity = world.host.replication.Spawn();
    world.Pump();
    const NetEntityID id = world.host.replication.GetNetworkID(entity);

    BinaryWriter forged;
    forged.Write(static_cast<std::uint8_t>(PacketType::ReplicationBatch));
    forged.Write(static_cast<std::uint8_t>(ReplicationOp::DestroyEntity));
    forged.Write(id);
    world.host.inbox.emplace_back(AliceId, forged.Buffer());
    world.Pump();
    CHECK(world.host.registry.valid(entity));
}

void TestPeerLeaveDestroysOwnedEntities()
{
    World world;
    RegisterAll(world.host.replication);
    Peer& alice = world.AddClient(AliceId, RegisterAll);
    Peer& bob = world.AddClient(BobId, RegisterAll);
    (void)alice;

    const entt::entity entity = world.host.replication.Spawn(AliceId);
    world.host.replication.Spawn(BobId);
    world.Pump();
    CHECK(bob.replication.NetworkEntityCount() == 2);

    world.host.replication.RemovePeer(AliceId);
    world.Pump();
    CHECK(!world.host.registry.valid(entity));
    CHECK(bob.replication.NetworkEntityCount() == 1);
}

void TestStopClearsClientWorld()
{
    World world;
    RegisterAll(world.host.replication);
    Peer& alice = world.AddClient(AliceId, RegisterAll);
    world.host.replication.Spawn();
    world.Pump();
    CHECK(alice.replication.NetworkEntityCount() == 1);

    alice.replication.Stop();
    CHECK(alice.replication.NetworkEntityCount() == 0);
    CHECK(alice.registry.view<NetworkEntity>().size() == 0);
}

struct Shoot
{
    float dirX = 0;
    float dirY = 0;
};

void TestMessages()
{
    MessageRegistry messages;
    PlayerID lastSender = 0;
    float lastDir = 0;
    messages.Register<Shoot>([&](PlayerID sender, const Shoot& shoot)
                             {
                                 lastSender = sender;
                                 lastDir = shoot.dirY;
                             },
                             {.delivery = Delivery::Unreliable, .direction = MessageDirection::ClientToHost});

    BinaryWriter writer;
    Delivery delivery = Delivery::Reliable;
    CHECK(messages.Encode(Shoot{1, 2}, writer, delivery));
    CHECK(delivery == Delivery::Unreliable);

    // Accepted in the allowed direction, with the sender supplied by the transport.
    BinaryReader fromClient(writer.Bytes());
    CHECK(messages.Dispatch(AliceId, false, fromClient));
    CHECK(lastSender == AliceId && lastDir == 2);

    // Rejected when it arrives from the host instead.
    lastSender = 0;
    BinaryReader fromHost(writer.Bytes());
    CHECK(!messages.Dispatch(HostId, true, fromHost));
    CHECK(lastSender == 0);

    // Unknown ids and truncated bodies are rejected.
    BinaryWriter unknown;
    unknown.Write(MessageTypeID{77});
    BinaryReader unknownReader(unknown.Bytes());
    CHECK(!messages.Dispatch(AliceId, false, unknownReader));
    BinaryReader truncated(writer.Bytes().data(), writer.Size() - 1);
    CHECK(!messages.Dispatch(AliceId, false, truncated));

    // Fingerprints differ when registrations differ.
    MessageRegistry other;
    CHECK(other.Fingerprint() != messages.Fingerprint());
}

void TestFingerprint()
{
    entt::registry a;
    entt::registry b;
    Replication left(a);
    Replication right(b);
    RegisterAll(left);
    RegisterAll(right);
    CHECK(left.Fingerprint() == right.Fingerprint());

    right.Register<Shoot>({});
    CHECK(left.Fingerprint() != right.Fingerprint());
}

} // namespace

int main()
{
    TestSerialization();
    TestReaderRejectsMalformedInput();
    TestSpawnAndReplicate();
    TestLateJoinSnapshot();
    TestAudiences();
    TestPeriodicSkipsUnchangedAndRefreshes();
    TestStaleUnreliableUpdatesAreDiscarded();
    TestClientAuthority();
    TestClientsCannotInjectOtherOps();
    TestPeerLeaveDestroysOwnedEntities();
    TestStopClearsClientWorld();
    TestMessages();
    TestFingerprint();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
