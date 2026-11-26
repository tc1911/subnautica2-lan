// SN2Lan - wire protocol helpers.
//
// Everything in this file is platform independent so that the framing and the
// fragment reassembly can be unit tested on Linux (see tests/test_protocol.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sn2lan {

// 'S' 'N' '2' 'T' when written little endian.
constexpr uint32_t kMagic = 0x54324E53u;
constexpr uint8_t kVersion = 1;

constexpr uint8_t kTypeData = 1;
constexpr uint8_t kTypeHello = 2;
// Set in FrameHeader::flags by the host once its listen server is up.
constexpr uint8_t kFlagHostReady = 0x01;

constexpr size_t kPuidLen = 32;      // EOS product user ids are 32 hex chars
constexpr size_t kPuidField = kPuidLen + 1;
constexpr size_t kMaxFragments = 128;

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t channel;
    uint8_t flags;
    uint32_t msgId;
    uint16_t fragIndex;
    uint16_t fragCount;
    char sender[kPuidField];  // NUL terminated product user id of the sender
    uint16_t payloadLen;
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 51, "unexpected FrameHeader layout");
static_assert(offsetof(FrameHeader, payloadLen) == 49, "unexpected FrameHeader layout");

// Splits `data` into MTU sized frames. `msgId` identifies the whole message so
// that fragments can be reassembled on the other side.
bool buildFrames(const std::string& senderPuid,
                 uint8_t channel,
                 const void* data,
                 size_t len,
                 size_t mtu,
                 uint32_t msgId,
                 std::vector<std::vector<uint8_t>>& out);

// Validates a datagram. On success `payload` points into `buf`.
bool parseFrame(const uint8_t* buf,
                size_t size,
                FrameHeader& header,
                const uint8_t*& payload,
                size_t& payloadLen);

// Returns true when `s` looks like an EOS product user id ("32 hex chars").
bool isValidPuid(const char* s);

class Reassembler {
public:
    // Adds one fragment. Returns true and fills `out` once the message is complete.
    bool add(const FrameHeader& header,
             const uint8_t* payload,
             size_t payloadLen,
             uint64_t nowMs,
             std::vector<uint8_t>& out);

    // Drops incomplete messages older than `timeoutMs`.
    void gc(uint64_t nowMs, uint64_t timeoutMs = 5000);

    void clear();

    size_t pending() const { return entries_.size(); }

    // Incomplete messages are capped so a hostile peer cannot exhaust memory
    // by starting thousands of fragmented messages.
    static constexpr size_t kMaxPending = 128;

private:
    struct Entry {
        uint16_t count = 0;
        uint16_t got = 0;
        uint8_t channel = 0;
        std::vector<std::vector<uint8_t>> parts;
        std::vector<uint8_t> have;
        size_t total = 0;
        uint64_t lastMs = 0;
    };

    std::map<std::string, Entry> entries_;
};

}  // namespace sn2lan
