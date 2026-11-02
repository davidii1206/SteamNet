#pragma once

#include "steamnet/Messages/Message.hpp"
#include "steamnet/Serialization/Serializer.hpp"
#include "steamnet/Types.hpp"

#include <cstdint>
#include <functional>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace steamnet {

// Typed one-shot messages / RPCs. Ids are assigned in registration order, so host and clients must register the
// same messages in the same order (the handshake fingerprint verifies this).
class MessageRegistry
{
public:
    template<typename T>
    void Register(std::function<void(PlayerID sender, const T&)> handler, MessageOptions options = {})
    {
        const std::type_index type(typeid(T));
        if (byType_.contains(type))
        {
            return;
        }

        Entry entry;
        entry.id = static_cast<MessageTypeID>(entries_.size());
        entry.options = std::move(options);
        if (entry.options.name.empty())
        {
            entry.options.name = typeid(T).name();
        }
        entry.dispatch = [handler = std::move(handler)](PlayerID sender, BinaryReader& reader)
        {
            T value = Serializer<T>::Read(reader);
            if (reader.Ok() && handler)
            {
                handler(sender, value);
            }
            return reader.Ok();
        };

        byType_.emplace(type, entry.id);
        entries_.push_back(std::move(entry));
    }

    template<typename T>
    bool IsRegistered() const
    {
        return byType_.contains(std::type_index(typeid(T)));
    }

    // Writes the message header and body. Returns false if T was never registered.
    template<typename T>
    bool Encode(const T& message, BinaryWriter& writer, Delivery& delivery) const
    {
        const auto found = byType_.find(std::type_index(typeid(T)));
        if (found == byType_.end())
        {
            return false;
        }
        const Entry& entry = entries_[found->second];
        writer.Write(entry.id);
        Serializer<T>::Write(writer, message);
        delivery = entry.options.delivery;
        return true;
    }

    // Reads the header and invokes the registered handler. `senderIsHost` tells which direction the message
    // arrived in, so that direction restrictions can be enforced. Returns false for unknown, disallowed or
    // malformed messages.
    bool Dispatch(PlayerID sender, bool senderIsHost, BinaryReader& reader) const;

    // Hash over (id, name, direction) of every registration.
    std::uint64_t Fingerprint() const;

    std::size_t Count() const noexcept { return entries_.size(); }

private:
    struct Entry
    {
        MessageTypeID id = 0;
        MessageOptions options;
        std::function<bool(PlayerID, BinaryReader&)> dispatch;
    };

    std::vector<Entry> entries_;
    std::unordered_map<std::type_index, MessageTypeID> byType_;
};

} // namespace steamnet
