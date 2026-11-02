#pragma once

#include "steamnet/Types.hpp"

namespace steamnet {

// Marks an entity as network-replicated. entt::entity values are local to one registry, so this id (assigned
// by the host) is what identifies the entity across machines. Added by Network::Spawn /
// Network::CreateNetworkEntity on the host and by the replication layer on clients.
struct NetworkEntity
{
    NetEntityID id = InvalidNetEntity;
};

// The player that owns the entity. Owners may submit updates to Authority::OwningClient components and are the
// recipients of Audience::OwnerOnly components. Ownership is assigned by the host only.
struct NetworkOwner
{
    PlayerID owner = InvalidPlayer;
};

} // namespace steamnet
