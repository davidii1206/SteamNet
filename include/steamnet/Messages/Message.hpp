#pragma once

#include "steamnet/Types.hpp"

#include <string>

namespace steamnet {

enum class MessageDirection : std::uint8_t
{
    ClientToHost,
    HostToClient,
    Both,
};

struct MessageOptions
{
    Delivery delivery = Delivery::Reliable;

    // Enforced on receipt: a message arriving in a direction that is not allowed is dropped.
    MessageDirection direction = MessageDirection::Both;

    // Stable name for the handshake fingerprint (see ComponentPolicy::name).
    std::string name{};
};

} // namespace steamnet
