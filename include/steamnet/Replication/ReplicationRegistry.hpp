#pragma once

#include "steamnet/Replication/ComponentPolicy.hpp"
#include "steamnet/Serialization/Serializer.hpp"
#include "steamnet/Types.hpp"

#include <entt/entity/registry.hpp>

#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace steamnet {

enum class ClientApplyResult : std::uint8_t
{
    Applied,
    Malformed,
    Rejected, // the game's validator refused the value
};

// Type-erased operations for one registered component type.
struct ComponentOps
{
    ComponentTypeID id = 0;
    ComponentPolicy policy;
    std::type_index type = std::type_index(typeid(void));

    // Writes the component of `entity`; false if the entity does not have it.
    std::function<bool(const entt::registry&, entt::entity, BinaryWriter&)> serialize;
    // Reads a value and emplaces/replaces it on `entity`.
    std::function<bool(entt::registry&, entt::entity, BinaryReader&)> apply;
    // Same, but runs the game's validator first (used by the host for client-submitted data).
    std::function<ClientApplyResult(entt::registry&, entt::entity, PlayerID, BinaryReader&)> applyFromClient;
    std::function<void(entt::registry&, entt::entity)> remove;
    std::function<bool(const entt::registry&, entt::entity)> has;
    // Appends every entity that has both NetworkEntity and this component.
    std::function<void(const entt::registry&, std::vector<entt::entity>&)> collect;
    std::function<void()> disconnectSignals;
    std::shared_ptr<void> signalTarget;

    // Accumulates time for UpdateMode::Periodic polling.
    float periodicTimer = 0.0f;
};

class ReplicationRegistry
{
public:
    ComponentOps& Add(std::unique_ptr<ComponentOps> ops)
    {
        ops->id = static_cast<ComponentTypeID>(components_.size());
        byType_.emplace(ops->type, ops->id);
        components_.push_back(std::move(ops));
        return *components_.back();
    }

    ComponentOps* Find(ComponentTypeID id) noexcept
    {
        return id < components_.size() ? components_[id].get() : nullptr;
    }

    const ComponentOps* Find(ComponentTypeID id) const noexcept
    {
        return id < components_.size() ? components_[id].get() : nullptr;
    }

    ComponentOps* Find(std::type_index type) noexcept
    {
        const auto it = byType_.find(type);
        return it == byType_.end() ? nullptr : components_[it->second].get();
    }

    std::size_t Count() const noexcept { return components_.size(); }

    auto begin() noexcept { return components_.begin(); }
    auto end() noexcept { return components_.end(); }
    auto begin() const noexcept { return components_.begin(); }
    auto end() const noexcept { return components_.end(); }

private:
    std::vector<std::unique_ptr<ComponentOps>> components_;
    std::unordered_map<std::type_index, ComponentTypeID> byType_;
};

} // namespace steamnet
