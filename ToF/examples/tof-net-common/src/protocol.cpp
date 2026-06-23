#include "tof_net/protocol.hpp"

namespace tof_net {

std::vector<uint8_t> pack_message(MessageType type, const void *payload, size_t payload_size) {
    MessageHeader header;
    header.type = static_cast<uint16_t>(type);
    header.payload_bytes = static_cast<uint64_t>(payload_size);

    std::vector<uint8_t> out(sizeof(MessageHeader) + payload_size);
    std::memcpy(out.data(), &header, sizeof(header));
    if (payload_size > 0) {
        if (!payload) {
            throw std::invalid_argument("payload pointer is null");
        }
        std::memcpy(out.data() + sizeof(MessageHeader), payload, payload_size);
    }
    return out;
}

Message unpack_message(const std::vector<uint8_t> &bytes) {
    if (bytes.size() < sizeof(MessageHeader)) {
        throw std::runtime_error("message too small");
    }
    MessageHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kMagic || header.version != kProtocolVersion) {
        throw std::runtime_error("invalid protocol header");
    }
    if (header.header_bytes != sizeof(MessageHeader)) {
        throw std::runtime_error("unsupported message header size");
    }
    if (bytes.size() != sizeof(MessageHeader) + header.payload_bytes) {
        throw std::runtime_error("message size mismatch");
    }
    Message msg;
    msg.type = static_cast<MessageType>(header.type);
    msg.payload.assign(bytes.begin() + sizeof(MessageHeader), bytes.end());
    return msg;
}

} // namespace tof_net
