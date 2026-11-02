#pragma once

#include "steamnet/Types.hpp"

#include <string>

namespace steamnet {

struct PlayerInfo
{
    PlayerID id = InvalidPlayer;
    std::string name;
    bool isHost = false;
    bool ready = false;
};

} // namespace steamnet
