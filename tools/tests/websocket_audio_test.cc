#include "protocols/websocket_audio.h"
#include <cassert>
#include <iostream>
int main() {
    const char v2[] = {0,2,0,0, 0,0,0,0, 0x12,0x34,0x56,0x78, 0,0,0,2, 1,2};
    for (size_t n = 0; n < sizeof(v2); ++n) assert(!DecodeWebsocketAudio(v2,n,2,24000,60));
    auto packet = DecodeWebsocketAudio(v2,sizeof(v2),2,24000,60);
    assert(packet && packet->timestamp == 0x12345678 && packet->payload == std::vector<uint8_t>({1,2}));
    assert(packet->sample_rate == 24000 && packet->frame_duration == 60 && !packet->pcm);
    const char v3[] = {0,0,0,2,1,2};
    for (size_t n = 0; n < sizeof(v3); ++n) assert(!DecodeWebsocketAudio(v3,n,3,16000,40));
    assert(DecodeWebsocketAudio(v3,sizeof(v3),3,16000,40)->payload.size() == 2);
    const char overflow[] = {0,0,char(0xff),char(0xff)};
    assert(!DecodeWebsocketAudio(overflow,sizeof(overflow),3,16000,40));
    auto raw = DecodeWebsocketAudio(v3,sizeof(v3),1,16000,40);
    assert(raw && raw->payload.size() == sizeof(v3));
    assert(!DecodeWebsocketAudio(nullptr,0,2,16000,40));
    std::cout << "WebSocket audio: truncated headers/payloads, oversized lengths, immutable frames, v1/v2/v3 PASS\n";
}
