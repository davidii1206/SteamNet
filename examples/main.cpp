#include "steamnet/Network.h"

#include <iostream>

int main() {
    steamnet::Network network;

    if (!network.initialize()) {
        std::cout << "Steamworks failed to initialize. This is expected when Steam is not running or no app ID is configured.\n";
        return 0;
    }

    auto& registry = network.registry();
    const auto entity = registry.create();
    registry.emplace<int>(entity, 42);

    std::cout << "Network initialized: " << network.name() << '\n';
    std::cout << "Entity value: " << registry.get<int>(entity) << '\n';

    network.shutdown();
    return 0;
}
