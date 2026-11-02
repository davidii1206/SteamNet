#include "steamnet/Messages/MessageRegistry.hpp"

#include "Hash.hpp"

namespace steamnet {

bool MessageRegistry::Dispatch(PlayerID sender, bool senderIsHost, BinaryReader& reader) const
{
    const MessageTypeID id = reader.ReadUInt16();
    if (!reader.Ok() || id >= entries_.size())
    {
        return false;
    }

    const Entry& entry = entries_[id];
    const MessageDirection direction = entry.options.direction;
    if (direction != MessageDirection::Both)
    {
        const MessageDirection arrived = senderIsHost ? MessageDirection::HostToClient : MessageDirection::ClientToHost;
        if (direction != arrived)
        {
            return false;
        }
    }

    return entry.dispatch(sender, reader);
}

std::uint64_t MessageRegistry::Fingerprint() const
{
    std::uint64_t hash = detail::FnvOffset;
    for (const Entry& entry : entries_)
    {
        hash = detail::FnvAppend(hash, entry.id);
        hash = detail::FnvAppend(hash, static_cast<std::uint8_t>(entry.options.direction));
        hash = detail::FnvAppend(hash, entry.options.name);
    }
    return hash;
}

} // namespace steamnet
