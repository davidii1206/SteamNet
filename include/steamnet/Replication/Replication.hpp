#pragma once

#include "steamnet/Replication/ComponentPolicy.hpp"
#include "steamnet/Replication/NetworkEntity.hpp"
#include "steamnet/Replication/ReplicationRegistry.hpp"
#include "steamnet/Serialization/BinaryReader.hpp"
#include "steamnet/Serialization/BinaryWriter.hpp"
#include "steamnet/Transport/Packet.hpp"
#include "steamnet/Types.hpp"

#include <entt/entity/registry.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace steamnet {

struct ReplicationConfig
{
    // Replication packets are produced at most this many times per second.
    float sendRate = 30.0f;

    // Host: destroy the entities owned by a player when that player leaves.
    bool destroyOwnedEntitiesOnDisconnect = true;

    // Unreliable batches are split to stay near one network MTU; reliable batches are split at this size.
    std::size_t maxUnreliablePacketSize = 1100;
    std::size_t maxReliablePacketSize = 60000;
};

// Host-authoritative replication of opted-in EnTT entities and registered components.
//
// The host owns entity creation/destruction. Components are replicated according to their ComponentPolicy:
//   - changes are detected through EnTT's on_construct/on_update/on_destroy signals (registry.patch,
//     replace, emplace_or_replace), through Edit<T>()/MarkDirty<T>(), or by polling (UpdateMode::Periodic);
//   - clients may only author components registered with Authority::OwningClient on entities they own, and
//     the host runs the optional validator before accepting anything.
//
// This class is transport agnostic: Network wires `SendFunction` to Steam, tests wire it to memory.
class Replication
{
public:
    using SendFunction = std::function<void(PlayerID to, std::span<const std::uint8_t> data, Delivery delivery)>;

    template<typename T>
    using Validator = std::function<bool(PlayerID sender, entt::entity entity, const T& value)>;

    explicit Replication(entt::registry& registry, ReplicationConfig config = {});
    ~Replication();

    Replication(const Replication&) = delete;
    Replication& operator=(const Replication&) = delete;

    // ---- Registration -------------------------------------------------------------------------------------

    // Registers T for replication. Host and clients must register the same components in the same order.
    // `validator` is only consulted on the host for Authority::OwningClient components.
    template<typename T>
    ComponentTypeID Register(ComponentPolicy policy = {}, Validator<T> validator = {});

    // ---- Network entities ---------------------------------------------------------------------------------

    // Host only. Creates a new networked entity (NetworkEntity + NetworkOwner). `owner` defaults to the host.
    entt::entity Spawn(PlayerID owner = InvalidPlayer);

    // Host only. Opts an existing local entity into replication and returns its network id.
    NetEntityID CreateNetworkEntity(entt::entity entity, PlayerID owner = InvalidPlayer);

    // Host only for networked entities (clients must not destroy replicated entities). Non-networked entities
    // are destroyed locally.
    bool Destroy(entt::entity entity);

    // Host only. Transfers ownership; OwnerOnly components follow the new owner.
    bool SetOwner(entt::entity entity, PlayerID newOwner);

    // Returns a mutable reference and marks the component dirty so that it is replicated.
    template<typename T>
    T& Edit(entt::entity entity);

    template<typename T>
    void MarkDirty(entt::entity entity);

    NetEntityID GetNetworkID(entt::entity entity) const;
    entt::entity Find(NetEntityID id) const;
    PlayerID OwnerOf(entt::entity entity) const;
    bool IsOwnedLocally(entt::entity entity) const;
    bool IsNetworked(entt::entity entity) const { return localToNet_.contains(entity); }
    std::size_t NetworkEntityCount() const noexcept { return netToLocal_.size(); }

    // ---- Events -------------------------------------------------------------------------------------------

    // Client: the initial snapshot has been applied.
    void OnSynchronized(std::function<void()> callback) { onSynchronized_ = std::move(callback); }
    // Host: a client acknowledged its snapshot.
    void OnPeerSynchronized(std::function<void(PlayerID)> callback) { onPeerSynchronized_ = std::move(callback); }

    // ---- Driven by Network --------------------------------------------------------------------------------

    void StartHost(PlayerID self, SendFunction send);
    void StartClient(PlayerID self, PlayerID host, SendFunction send);
    // Clients destroy their replicated entities; the host just strips the networking components.
    void Stop();

    // Host: starts replicating to `peer` and sends it a full baseline snapshot.
    void AddPeer(PlayerID peer);
    void RemovePeer(PlayerID peer);

    // `reader` is positioned after the packet type byte. Returns true if the packet type belongs to replication.
    bool HandlePacket(PlayerID sender, PacketType type, BinaryReader& reader);

    // Collects and sends pending changes, rate limited to ReplicationConfig::sendRate.
    void Flush(float deltaTime);
    // Same without the rate limit.
    void FlushNow();

    // Hash of the registered components and their policies, compared during the handshake.
    std::uint64_t Fingerprint() const;

    bool IsHost() const noexcept { return role_ == Role::Host; }
    bool IsClient() const noexcept { return role_ == Role::Client; }
    PlayerID LocalPlayer() const noexcept { return self_; }

private:
    enum class Role : std::uint8_t { None, Host, Client };

    struct PendingOp
    {
        enum class Kind : std::uint8_t { Create, Destroy, Add, Remove, SetOwner } kind = Kind::Create;
        NetEntityID id = InvalidNetEntity;
        ComponentTypeID component = 0;
        PlayerID oldOwner = InvalidPlayer;
    };

    struct Outgoing
    {
        BinaryWriter reliable;
        BinaryWriter unreliable;
    };

    struct PeerState
    {
        Outgoing out;
        bool synchronized = false;
    };

    struct SendState
    {
        std::uint64_t hash = 0;
        std::uint32_t sequence = 0;
        float lastSent = 0.0f;
        bool hashValid = false;
    };

    // Target of the EnTT signals of one component type.
    struct SignalTarget
    {
        Replication* owner = nullptr;
        ComponentTypeID component = 0;

        void OnConstruct(entt::registry&, entt::entity entity) { owner->OnComponentConstruct(component, entity); }
        void OnUpdate(entt::registry&, entt::entity entity) { owner->OnComponentUpdate(component, entity); }
        void OnDestroy(entt::registry&, entt::entity entity) { owner->OnComponentDestroy(component, entity); }
    };

    static std::uint64_t MakeKey(entt::entity entity, ComponentTypeID component) noexcept
    {
        return (static_cast<std::uint64_t>(entt::to_integral(entity)) << 16) | component;
    }

    // Signals
    void OnComponentConstruct(ComponentTypeID component, entt::entity entity);
    void OnComponentUpdate(ComponentTypeID component, entt::entity entity);
    void OnComponentDestroy(ComponentTypeID component, entt::entity entity);
    void OnNetworkEntityDestroyed(entt::registry&, entt::entity entity);
    void MarkDirtyById(ComponentTypeID component, entt::entity entity);

    // Sending
    void FlushHost();
    void FlushClient();
    void ProcessPendingOps();
    void ProcessDirtyHost();
    void ProcessPeriodicHost();
    void ProcessClientUpdates();
    void EmitAdd(ComponentOps& ops, NetEntityID id, entt::entity entity, PlayerID owner, PlayerID onlyTo);
    void EmitUpdate(ComponentOps& ops, NetEntityID id, PlayerID owner, std::span<const std::uint8_t> payload);
    void SendCorrection(PlayerID to, ComponentOps& ops, NetEntityID id, entt::entity entity);
    void Append(PlayerID peer, Outgoing& out, bool reliable, std::span<const std::uint8_t> entry);
    void AppendToAll(bool reliable, std::span<const std::uint8_t> entry);
    void SendBuffer(PlayerID peer, BinaryWriter& writer, bool reliable);
    void SendOutgoing(PlayerID peer, Outgoing& out);
    void EmitClientUpdate(ComponentOps& ops, NetEntityID id, std::span<const std::uint8_t> payload);
    void SendSnapshot(PlayerID peer, PeerState& state);
    static bool Wants(const ComponentPolicy& policy, PlayerID peer, PlayerID owner, bool isUpdate) noexcept;
    std::uint32_t NextSequence(NetEntityID id, ComponentTypeID component);
    void NoteSent(NetEntityID id, ComponentTypeID component, std::uint64_t hash);

    // Receiving
    void HandleBatch(PlayerID sender, BinaryReader& reader);
    void ApplyHostOps(BinaryReader& reader);
    void ApplyClientOps(PlayerID sender, BinaryReader& reader);
    bool IsNewerSequence(NetEntityID id, ComponentTypeID component, std::uint32_t sequence);

    void MapEntity(NetEntityID id, entt::entity entity);
    void Unmap(entt::entity entity);

    entt::registry& registry_;
    ReplicationConfig config_;
    ReplicationRegistry components_;

    Role role_ = Role::None;
    PlayerID self_ = InvalidPlayer;
    PlayerID host_ = InvalidPlayer;
    SendFunction send_;

    NetEntityID nextNetworkId_ = 1;
    std::unordered_map<NetEntityID, entt::entity> netToLocal_;
    std::unordered_map<entt::entity, NetEntityID> localToNet_;

    std::vector<PendingOp> pending_;
    std::unordered_set<std::uint64_t> dirty_;
    std::unordered_map<PlayerID, PeerState> peers_;
    Outgoing clientOut_;

    std::unordered_map<NetEntityID, std::unordered_map<ComponentTypeID, SendState>> sendState_;
    std::unordered_map<NetEntityID, std::unordered_map<ComponentTypeID, std::uint32_t>> receiveSequence_;

    bool applyingRemote_ = false;
    float time_ = 0.0f;
    float lastFlushTime_ = 0.0f;
    float flushDelta_ = 0.0f;
    float tickAccumulator_ = 0.0f;

    BinaryWriter scratch_;
    BinaryWriter payload_;
    std::vector<entt::entity> entityScratch_;

    std::function<void()> onSynchronized_;
    std::function<void(PlayerID)> onPeerSynchronized_;
};

// ---- Template implementation --------------------------------------------------------------------------------

template<typename T>
ComponentTypeID Replication::Register(ComponentPolicy policy, Validator<T> validator)
{
    static_assert(!std::is_same_v<T, NetworkEntity> && !std::is_same_v<T, NetworkOwner>,
                  "NetworkEntity and NetworkOwner are replicated by the library itself.");

    if (ComponentOps* existing = components_.Find(std::type_index(typeid(T))))
    {
        return existing->id;
    }

    auto ops = std::make_unique<ComponentOps>();
    ops->type = std::type_index(typeid(T));
    ops->policy = std::move(policy);
    if (ops->policy.name.empty())
    {
        ops->policy.name = typeid(T).name();
    }

    ops->serialize = [](const entt::registry& registry, entt::entity entity, BinaryWriter& writer)
    {
        if constexpr (std::is_empty_v<T>)
        {
            return registry.all_of<T>(entity);
        }
        else
        {
            const T* value = registry.try_get<T>(entity);
            if (value == nullptr)
            {
                return false;
            }
            Serializer<T>::Write(writer, *value);
            return true;
        }
    };

    ops->apply = [](entt::registry& registry, entt::entity entity, BinaryReader& reader)
    {
        T value = Serializer<T>::Read(reader);
        if (!reader.Ok())
        {
            return false;
        }
        if constexpr (std::is_empty_v<T>)
        {
            registry.emplace_or_replace<T>(entity);
        }
        else
        {
            registry.emplace_or_replace<T>(entity, std::move(value));
        }
        return true;
    };

    ops->applyFromClient = [validator = std::move(validator)](entt::registry& registry, entt::entity entity,
                                                              PlayerID sender, BinaryReader& reader)
    {
        T value = Serializer<T>::Read(reader);
        if (!reader.Ok())
        {
            return ClientApplyResult::Malformed;
        }
        if (validator && !validator(sender, entity, value))
        {
            return ClientApplyResult::Rejected;
        }
        if constexpr (std::is_empty_v<T>)
        {
            registry.emplace_or_replace<T>(entity);
        }
        else
        {
            registry.emplace_or_replace<T>(entity, std::move(value));
        }
        return ClientApplyResult::Applied;
    };

    ops->remove = [](entt::registry& registry, entt::entity entity) { registry.remove<T>(entity); };
    ops->has = [](const entt::registry& registry, entt::entity entity) { return registry.all_of<T>(entity); };
    ops->collect = [](const entt::registry& registry, std::vector<entt::entity>& out)
    {
        for (const entt::entity entity : registry.view<const NetworkEntity, const T>())
        {
            out.push_back(entity);
        }
    };

    ComponentOps& added = components_.Add(std::move(ops));

    auto target = std::make_shared<SignalTarget>();
    target->owner = this;
    target->component = added.id;
    registry_.on_construct<T>().template connect<&SignalTarget::OnConstruct>(*target);
    registry_.on_update<T>().template connect<&SignalTarget::OnUpdate>(*target);
    registry_.on_destroy<T>().template connect<&SignalTarget::OnDestroy>(*target);
    added.disconnectSignals = [this, raw = target.get()]
    {
        registry_.on_construct<T>().template disconnect<&SignalTarget::OnConstruct>(*raw);
        registry_.on_update<T>().template disconnect<&SignalTarget::OnUpdate>(*raw);
        registry_.on_destroy<T>().template disconnect<&SignalTarget::OnDestroy>(*raw);
    };
    added.signalTarget = std::move(target);

    return added.id;
}

template<typename T>
T& Replication::Edit(entt::entity entity)
{
    static_assert(!std::is_empty_v<T>, "Empty components have no data to edit.");
    MarkDirty<T>(entity);
    return registry_.get<T>(entity);
}

template<typename T>
void Replication::MarkDirty(entt::entity entity)
{
    if (const ComponentOps* ops = components_.Find(std::type_index(typeid(T))))
    {
        MarkDirtyById(ops->id, entity);
    }
}

} // namespace steamnet
