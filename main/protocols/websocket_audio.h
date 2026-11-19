#pragma once
#include "protocol.h"
#include <memory>

// Read unaligned wire bytes without casting away const or trusting peer lengths.
inline std::unique_ptr<AudioStreamPacket> DecodeWebsocketAudio(
        const char* data, size_t length, int version, int rate, int duration) {
    if (!data) return nullptr;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t header = 0, count = length;
    uint32_t timestamp = 0;
    auto read16 = [&](size_t at) { return (uint32_t(bytes[at]) << 8) | bytes[at + 1]; };
    if (version == 2) {
        header = sizeof(BinaryProtocol2);
        if (length < header) return nullptr;
        timestamp = (read16(8) << 16) | read16(10);
        count = (read16(12) << 16) | read16(14);
    } else if (version == 3) {
        header = sizeof(BinaryProtocol3);
        if (length < header) return nullptr;
        count = read16(2);
    }
    if (count != length - header) return nullptr;
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = rate;
    packet->frame_duration = duration;
    packet->timestamp = timestamp;
    packet->payload.assign(bytes + header, bytes + length);
    return packet;
}
