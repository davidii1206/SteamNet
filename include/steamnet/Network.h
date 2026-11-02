#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <entt/entity/registry.hpp>

namespace steamnet {

class Network {
public:
    Network();
    ~Network();

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    bool initialize(std::uint32_t app_id = 0);
    void shutdown() noexcept;
    bool is_initialized() const noexcept;
    void update() noexcept;

    entt::registry& registry() noexcept;
    const entt::registry& registry() const noexcept;

    std::string_view name() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace steamnet
