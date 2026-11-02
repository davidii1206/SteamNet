#include "steamnet/Replication/Replication.hpp"

#include "Hash.hpp"

#include <algorithm>
#include <cmath>

namespace steamnet {

namespace {

constexpr std::uint8_t Op(ReplicationOp op) noexcept
{
    return static_cast<std::uint8_t>(op);
}

void WriteCreate(BinaryWriter& writer, NetEntityID id, PlayerID owner)
{
    writer.Write(Op(ReplicationOp::CreateEntity));
    writer.Write(id);
    writer.Write(owner);
}

void WriteDestroy(BinaryWriter& writer, NetEntityID id)
{
    writer.Write(Op(ReplicationOp::DestroyEntity));
    writer.Write(id);
}

void WriteSetOwner(BinaryWriter& writer, NetEntityID id, PlayerID owner)
{
    writer.Write(Op(ReplicationOp::SetOwner));
    writer.Write(id);
    writer.Write(owner);
}

void WriteRemove(BinaryWriter& writer, NetEntityID id, ComponentTypeID component)
{
    writer.Write(Op(ReplicationOp::RemoveComponent));
    writer.Write(id);
    writer.Write(component);
}

void WriteComponent(BinaryWriter& writer, ReplicationOp op, NetEntityID id, ComponentTypeID component,
                    std::uint32_t sequence, std::span<const std::uint8_t> payload)
{
    writer.Write(Op(op));
    writer.Write(id);
    writer.Write(component);
    writer.Write(sequence);
    writer.Write(static_cast<std::uint32_t>(payload.size()));
    writer.WriteBytes(payload);
}

std::uint64_t HashBytes(std::span<const std::uint8_t> bytes)
{
    return detail::FnvAppend(detail::FnvOffset, bytes);
}

} // namespace

Replication::Replication(entt::registry& registry, ReplicationConfig config)
    : registry_(registry), config_(config)
{
    registry_.on_destroy<NetworkEntity>().connect<&Replication::OnNetworkEntityDestroyed>(*this);
}

Replication::~Replication()
{
    registry_.on_destroy<NetworkEntity>().disconnect<&Replication::OnNetworkEntityDestroyed>(*this);
    for (auto& ops : components_)
    {
        if (ops->disconnectSignals)
        {
            ops->disconnectSignals();
        }
    }
}

// ---- Entities -----------------------------------------------------------------------------------------------

entt::entity Replication::Spawn(PlayerID owner)
{
    if (role_ != Role::Host)
    {
        return entt::null;
    }
    const entt::entity entity = registry_.create();
    CreateNetworkEntity(entity, owner);
    return entity;
}

NetEntityID Replication::CreateNetworkEntity(entt::entity entity, PlayerID owner)
{
    if (role_ != Role::Host || !registry_.valid(entity))
    {
        return InvalidNetEntity;
    }
    if (const auto existing = localToNet_.find(entity); existing != localToNet_.end())
    {
        return existing->second;
    }

    if (owner == InvalidPlayer)
    {
        owner = self_;
    }

    const NetEntityID id = nextNetworkId_++;
    MapEntity(id, entity);
    registry_.emplace_or_replace<NetworkEntity>(entity, NetworkEntity{id});
    registry_.emplace_or_replace<NetworkOwner>(entity, NetworkOwner{owner});

    pending_.push_back({PendingOp::Kind::Create, id, 0, InvalidPlayer});
    for (const auto& ops : components_)
    {
        if (ops->has(registry_, entity))
        {
            pending_.push_back({PendingOp::Kind::Add, id, ops->id, InvalidPlayer});
        }
    }
    return id;
}

bool Replication::Destroy(entt::entity entity)
{
    if (!registry_.valid(entity))
    {
        return false;
    }
    if (localToNet_.contains(entity) && role_ != Role::Host)
    {
        return false;
    }
    registry_.destroy(entity);
    return true;
}

bool Replication::SetOwner(entt::entity entity, PlayerID newOwner)
{
    const auto found = localToNet_.find(entity);
    if (role_ != Role::Host || found == localToNet_.end())
    {
        return false;
    }
    const PlayerID oldOwner = OwnerOf(entity);
    if (oldOwner == newOwner)
    {
        return true;
    }
    registry_.emplace_or_replace<NetworkOwner>(entity, NetworkOwner{newOwner});
    pending_.push_back({PendingOp::Kind::SetOwner, found->second, 0, oldOwner});
    return true;
}

NetEntityID Replication::GetNetworkID(entt::entity entity) const
{
    const auto found = localToNet_.find(entity);
    return found == localToNet_.end() ? InvalidNetEntity : found->second;
}

entt::entity Replication::Find(NetEntityID id) const
{
    const auto found = netToLocal_.find(id);
    return found == netToLocal_.end() ? entt::entity{entt::null} : found->second;
}

PlayerID Replication::OwnerOf(entt::entity entity) const
{
    if (!registry_.valid(entity))
    {
        return InvalidPlayer;
    }
    const NetworkOwner* owner = registry_.try_get<NetworkOwner>(entity);
    return owner != nullptr ? owner->owner : InvalidPlayer;
}

bool Replication::IsOwnedLocally(entt::entity entity) const
{
    return self_ != InvalidPlayer && OwnerOf(entity) == self_;
}

void Replication::MapEntity(NetEntityID id, entt::entity entity)
{
    netToLocal_[id] = entity;
    localToNet_[entity] = id;
}

void Replication::Unmap(entt::entity entity)
{
    const auto found = localToNet_.find(entity);
    if (found == localToNet_.end())
    {
        return;
    }
    const NetEntityID id = found->second;
    netToLocal_.erase(id);
    localToNet_.erase(found);
    sendState_.erase(id);
    receiveSequence_.erase(id);

    const std::uint64_t index = entt::to_integral(entity);
    std::erase_if(dirty_, [index](std::uint64_t key) { return (key >> 16) == index; });
}

// ---- Change detection ---------------------------------------------------------------------------------------

void Replication::MarkDirtyById(ComponentTypeID component, entt::entity entity)
{
    if (applyingRemote_ || role_ == Role::None)
    {
        return;
    }
    const ComponentOps* ops = components_.Find(component);
    if (ops == nullptr || !localToNet_.contains(entity) || ops->policy.updateMode != UpdateMode::OnChange)
    {
        return;
    }
    if (role_ == Role::Client && ops->policy.authority != Authority::OwningClient)
    {
        return;
    }
    dirty_.insert(MakeKey(entity, component));
}

void Replication::OnComponentConstruct(ComponentTypeID component, entt::entity entity)
{
    if (applyingRemote_ || role_ == Role::None)
    {
        return;
    }
    const auto found = localToNet_.find(entity);
    if (found == localToNet_.end())
    {
        return;
    }
    if (role_ == Role::Host)
    {
        pending_.push_back({PendingOp::Kind::Add, found->second, component, InvalidPlayer});
    }
    else
    {
        MarkDirtyById(component, entity);
    }
}

void Replication::OnComponentUpdate(ComponentTypeID component, entt::entity entity)
{
    MarkDirtyById(component, entity);
}

void Replication::OnComponentDestroy(ComponentTypeID component, entt::entity entity)
{
    if (applyingRemote_ || role_ != Role::Host)
    {
        return;
    }
    const auto found = localToNet_.find(entity);
    if (found == localToNet_.end())
    {
        return;
    }
    dirty_.erase(MakeKey(entity, component));
    pending_.push_back({PendingOp::Kind::Remove, found->second, component, InvalidPlayer});
}

void Replication::OnNetworkEntityDestroyed(entt::registry&, entt::entity entity)
{
    const auto found = localToNet_.find(entity);
    if (found == localToNet_.end())
    {
        return;
    }
    if (role_ == Role::Host && !applyingRemote_)
    {
        pending_.push_back({PendingOp::Kind::Destroy, found->second, 0, InvalidPlayer});
    }
    Unmap(entity);
}

// ---- Session ------------------------------------------------------------------------------------------------

void Replication::StartHost(PlayerID self, SendFunction send)
{
    Stop();
    role_ = Role::Host;
    self_ = self;
    host_ = self;
    send_ = std::move(send);
}

void Replication::StartClient(PlayerID self, PlayerID host, SendFunction send)
{
    Stop();
    role_ = Role::Client;
    self_ = self;
    host_ = host;
    send_ = std::move(send);
}

void Replication::Stop()
{
    const Role previous = role_;
    role_ = Role::None;

    if (previous == Role::Client)
    {
        applyingRemote_ = true;
        const auto replicated = registry_.view<NetworkEntity>();
        const std::vector<entt::entity> entities(replicated.begin(), replicated.end());
        for (const entt::entity entity : entities)
        {
            registry_.destroy(entity);
        }
        applyingRemote_ = false;
    }
    else if (previous == Role::Host)
    {
        registry_.clear<NetworkOwner>();
        registry_.clear<NetworkEntity>();
    }

    netToLocal_.clear();
    localToNet_.clear();
    pending_.clear();
    dirty_.clear();
    peers_.clear();
    clientOut_ = {};
    sendState_.clear();
    receiveSequence_.clear();
    nextNetworkId_ = 1;
    self_ = InvalidPlayer;
    host_ = InvalidPlayer;
    send_ = nullptr;
    time_ = lastFlushTime_ = flushDelta_ = tickAccumulator_ = 0.0f;
    for (auto& ops : components_)
    {
        ops->periodicTimer = 0.0f;
    }
}

void Replication::AddPeer(PlayerID peer)
{
    if (role_ != Role::Host || peer == self_)
    {
        return;
    }
    PeerState& state = peers_[peer];
    state = PeerState{};
    SendSnapshot(peer, state);
}

void Replication::RemovePeer(PlayerID peer)
{
    if (peers_.erase(peer) == 0 || role_ != Role::Host || !config_.destroyOwnedEntitiesOnDisconnect)
    {
        return;
    }

    std::vector<entt::entity> owned;
    for (const auto entity : registry_.view<NetworkOwner>())
    {
        if (registry_.get<NetworkOwner>(entity).owner == peer)
        {
            owned.push_back(entity);
        }
    }
    for (const entt::entity entity : owned)
    {
        registry_.destroy(entity);
    }
}

std::uint64_t Replication::Fingerprint() const
{
    std::uint64_t hash = detail::FnvAppend(detail::FnvOffset, ProtocolVersion);
    for (const auto& ops : components_)
    {
        hash = detail::FnvAppend(hash, ops->id);
        hash = detail::FnvAppend(hash, ops->policy.name);
        hash = detail::FnvAppend(hash, static_cast<std::uint8_t>(ops->policy.authority));
        hash = detail::FnvAppend(hash, static_cast<std::uint8_t>(ops->policy.audience));
        hash = detail::FnvAppend(hash, ops->policy.schemaVersion);
    }
    return hash;
}

// ---- Sending ------------------------------------------------------------------------------------------------

void Replication::Flush(float deltaTime)
{
    if (role_ == Role::None)
    {
        return;
    }
    time_ += deltaTime;
    tickAccumulator_ += deltaTime;

    const float interval = config_.sendRate > 0.0f ? 1.0f / config_.sendRate : 0.0f;
    if (tickAccumulator_ < interval)
    {
        return;
    }
    tickAccumulator_ = interval > 0.0f ? std::fmod(tickAccumulator_, interval) : 0.0f;
    FlushNow();
}

void Replication::FlushNow()
{
    if (role_ == Role::None)
    {
        return;
    }
    flushDelta_ = time_ - lastFlushTime_;
    lastFlushTime_ = time_;

    if (role_ == Role::Host)
    {
        FlushHost();
    }
    else
    {
        FlushClient();
    }
}

bool Replication::Wants(const ComponentPolicy& policy, PlayerID peer, PlayerID owner, bool isUpdate) noexcept
{
    switch (policy.audience)
    {
    case Audience::HostOnly:
        return false;
    case Audience::OwnerOnly:
        if (peer != owner)
        {
            return false;
        }
        break;
    case Audience::Everyone:
        break;
    }
    // A client-authored value is never echoed back to its author; the author already has it.
    if (isUpdate && policy.authority == Authority::OwningClient && peer == owner)
    {
        return false;
    }
    return true;
}

std::uint32_t Replication::NextSequence(NetEntityID id, ComponentTypeID component)
{
    return ++sendState_[id][component].sequence;
}

void Replication::NoteSent(NetEntityID id, ComponentTypeID component, std::uint64_t hash)
{
    SendState& state = sendState_[id][component];
    state.hash = hash;
    state.hashValid = true;
    state.lastSent = time_;
}

void Replication::SendBuffer(PlayerID peer, BinaryWriter& writer, bool reliable)
{
    if (!writer.Empty() && send_)
    {
        send_(peer, writer.Bytes(), reliable ? Delivery::Reliable : Delivery::Unreliable);
    }
    writer.Clear();
}

void Replication::SendOutgoing(PlayerID peer, Outgoing& out)
{
    SendBuffer(peer, out.reliable, true);
    SendBuffer(peer, out.unreliable, false);
}

void Replication::Append(PlayerID peer, Outgoing& out, bool reliable, std::span<const std::uint8_t> entry)
{
    BinaryWriter& writer = reliable ? out.reliable : out.unreliable;
    const std::size_t limit = reliable ? config_.maxReliablePacketSize : config_.maxUnreliablePacketSize;
    if (!writer.Empty() && writer.Size() + entry.size() > limit)
    {
        SendBuffer(peer, writer, reliable);
    }
    if (writer.Empty())
    {
        writer.Write(static_cast<std::uint8_t>(PacketType::ReplicationBatch));
    }
    writer.WriteBytes(entry);
}

void Replication::AppendToAll(bool reliable, std::span<const std::uint8_t> entry)
{
    for (auto& [peer, state] : peers_)
    {
        Append(peer, state.out, reliable, entry);
    }
}

void Replication::EmitAdd(ComponentOps& ops, NetEntityID id, entt::entity entity, PlayerID owner, PlayerID onlyTo)
{
    payload_.Clear();
    if (!ops.serialize(registry_, entity, payload_))
    {
        return;
    }
    scratch_.Clear();
    WriteComponent(scratch_, ReplicationOp::AddComponent, id, ops.id, 0, payload_.Bytes());

    for (auto& [peer, state] : peers_)
    {
        if ((onlyTo != InvalidPlayer && peer != onlyTo) || !Wants(ops.policy, peer, owner, false))
        {
            continue;
        }
        Append(peer, state.out, true, scratch_.Bytes());
    }
    NoteSent(id, ops.id, HashBytes(payload_.Bytes()));
    dirty_.erase(MakeKey(entity, ops.id));
}

void Replication::EmitUpdate(ComponentOps& ops, NetEntityID id, PlayerID owner, std::span<const std::uint8_t> payload)
{
    scratch_.Clear();
    WriteComponent(scratch_, ReplicationOp::UpdateComponent, id, ops.id, NextSequence(id, ops.id), payload);

    const bool reliable = ops.policy.delivery == Delivery::Reliable;
    for (auto& [peer, state] : peers_)
    {
        if (Wants(ops.policy, peer, owner, true))
        {
            Append(peer, state.out, reliable, scratch_.Bytes());
        }
    }
}

void Replication::EmitClientUpdate(ComponentOps& ops, NetEntityID id, std::span<const std::uint8_t> payload)
{
    scratch_.Clear();
    WriteComponent(scratch_, ReplicationOp::UpdateComponent, id, ops.id, NextSequence(id, ops.id), payload);
    Append(host_, clientOut_, ops.policy.delivery == Delivery::Reliable, scratch_.Bytes());
}

void Replication::SendCorrection(PlayerID to, ComponentOps& ops, NetEntityID id, entt::entity entity)
{
    const auto peer = peers_.find(to);
    if (peer == peers_.end())
    {
        return;
    }

    scratch_.Clear();
    payload_.Clear();
    if (ops.serialize(registry_, entity, payload_))
    {
        WriteComponent(scratch_, ReplicationOp::UpdateComponent, id, ops.id, NextSequence(id, ops.id), payload_.Bytes());
    }
    else
    {
        WriteRemove(scratch_, id, ops.id);
    }
    Append(to, peer->second.out, true, scratch_.Bytes());
}

void Replication::SendSnapshot(PlayerID peer, PeerState& state)
{
    Outgoing& out = state.out;

    for (const auto& [id, entity] : netToLocal_)
    {
        scratch_.Clear();
        WriteCreate(scratch_, id, OwnerOf(entity));
        Append(peer, out, true, scratch_.Bytes());
    }

    for (const auto& ops : components_)
    {
        entityScratch_.clear();
        ops->collect(registry_, entityScratch_);
        for (const entt::entity entity : entityScratch_)
        {
            const auto found = localToNet_.find(entity);
            if (found == localToNet_.end())
            {
                continue;
            }
            const PlayerID owner = OwnerOf(entity);
            if (!Wants(ops->policy, peer, owner, false))
            {
                continue;
            }
            payload_.Clear();
            if (!ops->serialize(registry_, entity, payload_))
            {
                continue;
            }
            scratch_.Clear();
            WriteComponent(scratch_, ReplicationOp::AddComponent, found->second, ops->id, 0, payload_.Bytes());
            Append(peer, out, true, scratch_.Bytes());
        }
    }
    SendBuffer(peer, out.reliable, true);

    scratch_.Clear();
    scratch_.Write(static_cast<std::uint8_t>(PacketType::SnapshotEnd));
    scratch_.Write(static_cast<std::uint32_t>(netToLocal_.size()));
    if (send_)
    {
        send_(peer, scratch_.Bytes(), Delivery::Reliable);
    }
}

void Replication::FlushHost()
{
    if (!peers_.empty())
    {
        ProcessPendingOps();
        ProcessDirtyHost();
        ProcessPeriodicHost();
        for (auto& [peer, state] : peers_)
        {
            SendOutgoing(peer, state.out);
        }
    }
    else
    {
        // Nobody to tell yet; a joining client gets a full snapshot instead.
        pending_.clear();
        dirty_.clear();
    }
}

void Replication::ProcessPendingOps()
{
    std::unordered_set<NetEntityID> destroyed;
    for (const PendingOp& op : pending_)
    {
        if (op.kind == PendingOp::Kind::Destroy)
        {
            destroyed.insert(op.id);
        }
    }

    // Operations may enqueue more work through signals (never expected), so iterate a moved-out copy.
    std::vector<PendingOp> operations = std::move(pending_);
    pending_.clear();

    for (const PendingOp& op : operations)
    {
        const entt::entity entity = Find(op.id);
        switch (op.kind)
        {
        case PendingOp::Kind::Create:
            if (entity != entt::null)
            {
                scratch_.Clear();
                WriteCreate(scratch_, op.id, OwnerOf(entity));
                AppendToAll(true, scratch_.Bytes());
            }
            break;

        case PendingOp::Kind::Destroy:
            scratch_.Clear();
            WriteDestroy(scratch_, op.id);
            AppendToAll(true, scratch_.Bytes());
            break;

        case PendingOp::Kind::Add:
            if (entity != entt::null)
            {
                if (ComponentOps* ops = components_.Find(op.component))
                {
                    EmitAdd(*ops, op.id, entity, OwnerOf(entity), InvalidPlayer);
                }
            }
            break;

        case PendingOp::Kind::Remove:
            if (!destroyed.contains(op.id))
            {
                if (ComponentOps* ops = components_.Find(op.component))
                {
                    const PlayerID owner = entity != entt::null ? OwnerOf(entity) : InvalidPlayer;
                    scratch_.Clear();
                    WriteRemove(scratch_, op.id, op.component);
                    for (auto& [peer, state] : peers_)
                    {
                        if (Wants(ops->policy, peer, owner, false))
                        {
                            Append(peer, state.out, true, scratch_.Bytes());
                        }
                    }
                    if (const auto states = sendState_.find(op.id); states != sendState_.end())
                    {
                        states->second.erase(op.component);
                    }
                }
            }
            break;

        case PendingOp::Kind::SetOwner:
            if (entity != entt::null)
            {
                const PlayerID newOwner = OwnerOf(entity);
                scratch_.Clear();
                WriteSetOwner(scratch_, op.id, newOwner);
                AppendToAll(true, scratch_.Bytes());

                // OwnerOnly components follow the owner.
                for (auto& ops : components_)
                {
                    if (ops->policy.audience != Audience::OwnerOnly || !ops->has(registry_, entity))
                    {
                        continue;
                    }
                    if (const auto oldPeer = peers_.find(op.oldOwner); oldPeer != peers_.end())
                    {
                        scratch_.Clear();
                        WriteRemove(scratch_, op.id, ops->id);
                        Append(op.oldOwner, oldPeer->second.out, true, scratch_.Bytes());
                    }
                    if (peers_.contains(newOwner))
                    {
                        EmitAdd(*ops, op.id, entity, newOwner, newOwner);
                    }
                }
            }
            break;
        }
    }
}

void Replication::ProcessDirtyHost()
{
    std::unordered_set<std::uint64_t> dirty = std::move(dirty_);
    dirty_.clear();

    for (const std::uint64_t key : dirty)
    {
        const auto entity = static_cast<entt::entity>(static_cast<std::underlying_type_t<entt::entity>>(key >> 16));
        const auto component = static_cast<ComponentTypeID>(key & 0xFFFFu);

        const auto found = localToNet_.find(entity);
        ComponentOps* ops = components_.Find(component);
        if (found == localToNet_.end() || ops == nullptr || !registry_.valid(entity))
        {
            continue;
        }

        payload_.Clear();
        if (!ops->serialize(registry_, entity, payload_))
        {
            continue;
        }
        NoteSent(found->second, component, HashBytes(payload_.Bytes()));
        EmitUpdate(*ops, found->second, OwnerOf(entity), payload_.Bytes());
    }
}

void Replication::ProcessPeriodicHost()
{
    for (auto& ops : components_)
    {
        if (ops->policy.updateMode != UpdateMode::Periodic || ops->policy.audience == Audience::HostOnly)
        {
            continue;
        }

        ops->periodicTimer += flushDelta_;
        const float period = ops->policy.periodicHz > 0.0f ? 1.0f / ops->policy.periodicHz : 0.0f;
        if (ops->periodicTimer < period)
        {
            continue;
        }
        ops->periodicTimer = period > 0.0f ? std::fmod(ops->periodicTimer, period) : 0.0f;

        entityScratch_.clear();
        ops->collect(registry_, entityScratch_);
        for (const entt::entity entity : entityScratch_)
        {
            const auto found = localToNet_.find(entity);
            if (found == localToNet_.end())
            {
                continue;
            }

            payload_.Clear();
            if (!ops->serialize(registry_, entity, payload_))
            {
                continue;
            }

            const std::uint64_t hash = HashBytes(payload_.Bytes());
            SendState& state = sendState_[found->second][ops->id];
            const bool changed = !state.hashValid || state.hash != hash;
            const bool refresh = ops->policy.refreshInterval > 0.0f && (time_ - state.lastSent) >= ops->policy.refreshInterval;
            if (!changed && !refresh)
            {
                continue;
            }

            state.hash = hash;
            state.hashValid = true;
            state.lastSent = time_;
            EmitUpdate(*ops, found->second, OwnerOf(entity), payload_.Bytes());
        }
    }
}

void Replication::FlushClient()
{
    ProcessClientUpdates();
    SendOutgoing(host_, clientOut_);
}

void Replication::ProcessClientUpdates()
{
    std::unordered_set<std::uint64_t> dirty = std::move(dirty_);
    dirty_.clear();

    for (const std::uint64_t key : dirty)
    {
        const auto entity = static_cast<entt::entity>(static_cast<std::underlying_type_t<entt::entity>>(key >> 16));
        const auto component = static_cast<ComponentTypeID>(key & 0xFFFFu);

        const auto found = localToNet_.find(entity);
        ComponentOps* ops = components_.Find(component);
        if (found == localToNet_.end() || ops == nullptr || !registry_.valid(entity) || !IsOwnedLocally(entity))
        {
            continue;
        }

        payload_.Clear();
        if (!ops->serialize(registry_, entity, payload_))
        {
            continue;
        }
        NoteSent(found->second, component, HashBytes(payload_.Bytes()));
        EmitClientUpdate(*ops, found->second, payload_.Bytes());
    }

    for (auto& ops : components_)
    {
        if (ops->policy.authority != Authority::OwningClient || ops->policy.updateMode != UpdateMode::Periodic)
        {
            continue;
        }

        ops->periodicTimer += flushDelta_;
        const float period = ops->policy.periodicHz > 0.0f ? 1.0f / ops->policy.periodicHz : 0.0f;
        if (ops->periodicTimer < period)
        {
            continue;
        }
        ops->periodicTimer = period > 0.0f ? std::fmod(ops->periodicTimer, period) : 0.0f;

        entityScratch_.clear();
        ops->collect(registry_, entityScratch_);
        for (const entt::entity entity : entityScratch_)
        {
            const auto found = localToNet_.find(entity);
            if (found == localToNet_.end() || !IsOwnedLocally(entity))
            {
                continue;
            }

            payload_.Clear();
            if (!ops->serialize(registry_, entity, payload_))
            {
                continue;
            }

            const std::uint64_t hash = HashBytes(payload_.Bytes());
            SendState& state = sendState_[found->second][ops->id];
            const bool changed = !state.hashValid || state.hash != hash;
            const bool refresh = ops->policy.refreshInterval > 0.0f && (time_ - state.lastSent) >= ops->policy.refreshInterval;
            if (!changed && !refresh)
            {
                continue;
            }

            state.hash = hash;
            state.hashValid = true;
            state.lastSent = time_;
            EmitClientUpdate(*ops, found->second, payload_.Bytes());
        }
    }
}

// ---- Receiving ----------------------------------------------------------------------------------------------

bool Replication::HandlePacket(PlayerID sender, PacketType type, BinaryReader& reader)
{
    switch (type)
    {
    case PacketType::ReplicationBatch:
        HandleBatch(sender, reader);
        return true;

    case PacketType::SnapshotEnd:
        if (role_ == Role::Client && sender == host_)
        {
            const std::uint8_t ack = static_cast<std::uint8_t>(PacketType::SnapshotAck);
            if (send_)
            {
                send_(host_, std::span<const std::uint8_t>(&ack, 1), Delivery::Reliable);
            }
            if (onSynchronized_)
            {
                onSynchronized_();
            }
        }
        return true;

    case PacketType::SnapshotAck:
        if (role_ == Role::Host)
        {
            if (const auto peer = peers_.find(sender); peer != peers_.end() && !peer->second.synchronized)
            {
                peer->second.synchronized = true;
                if (onPeerSynchronized_)
                {
                    onPeerSynchronized_(sender);
                }
            }
        }
        return true;

    default:
        return false;
    }
}

void Replication::HandleBatch(PlayerID sender, BinaryReader& reader)
{
    if (role_ == Role::Client && sender == host_)
    {
        ApplyHostOps(reader);
    }
    else if (role_ == Role::Host && peers_.contains(sender))
    {
        ApplyClientOps(sender, reader);
    }
}

bool Replication::IsNewerSequence(NetEntityID id, ComponentTypeID component, std::uint32_t sequence)
{
    auto& slots = receiveSequence_[id];
    const auto found = slots.find(component);
    if (found == slots.end())
    {
        slots.emplace(component, sequence);
        return true;
    }
    if (static_cast<std::int32_t>(sequence - found->second) > 0)
    {
        found->second = sequence;
        return true;
    }
    return false;
}

void Replication::ApplyHostOps(BinaryReader& reader)
{
    while (reader.Ok() && !reader.AtEnd())
    {
        const auto op = static_cast<ReplicationOp>(reader.ReadUInt8());
        const NetEntityID id = reader.ReadUInt64();

        switch (op)
        {
        case ReplicationOp::CreateEntity:
        {
            const PlayerID owner = reader.ReadUInt64();
            if (!reader.Ok())
            {
                return;
            }
            applyingRemote_ = true;
            entt::entity entity = Find(id);
            if (entity == entt::null)
            {
                entity = registry_.create();
                MapEntity(id, entity);
                registry_.emplace<NetworkEntity>(entity, NetworkEntity{id});
            }
            registry_.emplace_or_replace<NetworkOwner>(entity, NetworkOwner{owner});
            applyingRemote_ = false;
            break;
        }

        case ReplicationOp::DestroyEntity:
        {
            if (!reader.Ok())
            {
                return;
            }
            if (const entt::entity entity = Find(id); entity != entt::null)
            {
                applyingRemote_ = true;
                registry_.destroy(entity);
                applyingRemote_ = false;
            }
            break;
        }

        case ReplicationOp::AddComponent:
        case ReplicationOp::UpdateComponent:
        {
            const ComponentTypeID component = reader.ReadUInt16();
            const std::uint32_t sequence = reader.ReadUInt32();
            const std::uint32_t length = reader.ReadUInt32();
            BinaryReader payload = reader.Slice(length);
            if (!reader.Ok())
            {
                return;
            }

            const entt::entity entity = Find(id);
            ComponentOps* ops = components_.Find(component);
            if (entity == entt::null || ops == nullptr)
            {
                break;
            }

            if (op == ReplicationOp::UpdateComponent)
            {
                if (!IsNewerSequence(id, component, sequence))
                {
                    break;
                }
            }
            else if (const auto slots = receiveSequence_.find(id); slots != receiveSequence_.end())
            {
                slots->second.erase(component);
            }

            applyingRemote_ = true;
            ops->apply(registry_, entity, payload);
            applyingRemote_ = false;
            break;
        }

        case ReplicationOp::RemoveComponent:
        {
            const ComponentTypeID component = reader.ReadUInt16();
            if (!reader.Ok())
            {
                return;
            }
            const entt::entity entity = Find(id);
            if (ComponentOps* ops = components_.Find(component); ops != nullptr && entity != entt::null)
            {
                applyingRemote_ = true;
                ops->remove(registry_, entity);
                applyingRemote_ = false;
            }
            break;
        }

        case ReplicationOp::SetOwner:
        {
            const PlayerID owner = reader.ReadUInt64();
            if (!reader.Ok())
            {
                return;
            }
            if (const entt::entity entity = Find(id); entity != entt::null)
            {
                applyingRemote_ = true;
                registry_.emplace_or_replace<NetworkOwner>(entity, NetworkOwner{owner});
                applyingRemote_ = false;
            }
            break;
        }

        default:
            return;
        }
    }
}

void Replication::ApplyClientOps(PlayerID sender, BinaryReader& reader)
{
    while (reader.Ok() && !reader.AtEnd())
    {
        // Clients may only submit component updates; anything else is a protocol violation.
        const auto op = static_cast<ReplicationOp>(reader.ReadUInt8());
        if (op != ReplicationOp::UpdateComponent)
        {
            return;
        }

        const NetEntityID id = reader.ReadUInt64();
        const ComponentTypeID component = reader.ReadUInt16();
        const std::uint32_t sequence = reader.ReadUInt32();
        const std::uint32_t length = reader.ReadUInt32();
        BinaryReader payload = reader.Slice(length);
        if (!reader.Ok())
        {
            return;
        }

        const entt::entity entity = Find(id);
        ComponentOps* ops = components_.Find(component);
        if (entity == entt::null || ops == nullptr)
        {
            continue;
        }
        // Ownership and authority come from the host's own state, never from the packet.
        if (OwnerOf(entity) != sender || ops->policy.authority != Authority::OwningClient)
        {
            continue;
        }
        if (!IsNewerSequence(id, component, sequence))
        {
            continue;
        }

        if (ops->applyFromClient(registry_, entity, sender, payload) == ClientApplyResult::Rejected)
        {
            SendCorrection(sender, *ops, id, entity);
        }
    }
}

} // namespace steamnet
