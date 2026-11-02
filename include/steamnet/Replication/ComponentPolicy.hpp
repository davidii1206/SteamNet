#pragma once

#include "steamnet/Types.hpp"

#include <cstdint>
#include <string>

namespace steamnet {

// Declares how a component type is replicated. Field order matters for designated initializers:
//   network.Replication().Register<Health>({
//       .authority = Authority::Host,
//       .audience = Audience::Everyone,
//       .delivery = Delivery::Reliable,
//       .updateMode = UpdateMode::OnChange});
struct ComponentPolicy
{
    Authority authority = Authority::Host;
    Audience audience = Audience::Everyone;
    Delivery delivery = Delivery::Reliable;
    UpdateMode updateMode = UpdateMode::OnChange;

    // UpdateMode::Periodic: how often the component is polled.
    float periodicHz = 20.0f;

    // UpdateMode::Periodic: unchanged values are re-sent after this many seconds so that a lost unreliable
    // packet cannot leave a client stale forever. 0 disables refreshes.
    float refreshInterval = 1.0f;

    // Bump when the serialized layout of the component changes; part of the handshake fingerprint.
    std::uint16_t schemaVersion = 1;

    // Stable name used for the handshake fingerprint. Defaults to typeid(T).name(), which is only portable
    // between identical builds; set it explicitly when host and clients run different compilers/platforms.
    std::string name{};
};

} // namespace steamnet
