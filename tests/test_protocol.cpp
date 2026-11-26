// Native unit tests for the wire protocol. Build & run with scripts/test.sh.
#include "../src/protocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace sn2lan;

namespace {

const char* kPuidA = "00020328bb49410db003bc886034d14d";
const char* kPuidB = "ffffffffffffffffffffffffffffffff";

std::vector<uint8_t> makePayload(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);
    }
    return v;
}

void roundTrip(size_t len, size_t mtu) {
    auto payload = makePayload(len);
    std::vector<std::vector<uint8_t>> frames;
    const bool built = buildFrames(kPuidA, 3, payload.empty() ? nullptr : payload.data(), len, mtu, 42, frames);
    assert(built);
    assert(!frames.empty());

    Reassembler reasm;
    std::vector<uint8_t> out;
    bool complete = false;
    uint64_t now = 1000;
    for (const auto& frame : frames) {
        assert(frame.size() <= mtu);
        FrameHeader header{};
        const uint8_t* data = nullptr;
        size_t dataLen = 0;
        assert(parseFrame(frame.data(), frame.size(), header, data, dataLen));
        assert(header.msgId == 42);
        assert(header.channel == 3);
        assert(std::strcmp(header.sender, kPuidA) == 0);
        bool done = reasm.add(header, data, dataLen, now++, out);
        if (done) {
            complete = true;
        }
    }
    assert(complete);
    assert(out.size() == len);
    if (len > 0) {
        assert(std::memcmp(out.data(), payload.data(), len) == 0);
    }
    assert(reasm.pending() == 0);
}

void testFragmentation() {
    roundTrip(0, 1400);
    roundTrip(1, 1400);
    roundTrip(1400 - 51, 1400);
    roundTrip(1400 - 50, 1400);
    roundTrip(5000, 1400);
    roundTrip(5000, 512);
    roundTrip(64 * 1024, 1400);
}

void testInvalidFrames() {
    auto payload = makePayload(100);
    std::vector<std::vector<uint8_t>> frames;
    assert(buildFrames(kPuidA, 0, payload.data(), payload.size(), 1400, 1, frames));
    assert(frames.size() == 1);

    FrameHeader header{};
    const uint8_t* data = nullptr;
    size_t len = 0;

    auto bad = frames[0];
    bad[0] ^= 0xff;
    assert(!parseFrame(bad.data(), bad.size(), header, data, len));

    bad = frames[0];
    bad[sizeof(FrameHeader) + 5] = 0;  // trailing garbage is allowed, payloadLen wins
    assert(parseFrame(bad.data(), bad.size(), header, data, len));

    bad = frames[0];
    assert(!parseFrame(bad.data(), sizeof(FrameHeader) - 1, header, data, len));

    // Too short payload (header claims more bytes than present).
    bad = frames[0];
    FrameHeader* h = reinterpret_cast<FrameHeader*>(bad.data());
    h->payloadLen = 200;
    assert(!parseFrame(bad.data(), bad.size(), header, data, len));

    // Bad sender.
    bad = frames[0];
    h = reinterpret_cast<FrameHeader*>(bad.data());
    h->sender[0] = 'z';
    assert(!parseFrame(bad.data(), bad.size(), header, data, len));

    // Fragment index out of range.
    bad = frames[0];
    h = reinterpret_cast<FrameHeader*>(bad.data());
    h->fragCount = 2;
    h->fragIndex = 2;
    assert(!parseFrame(bad.data(), bad.size(), header, data, len));
}

void testReassemblerDedupAndTimeout() {
    const size_t len = 4000;
    auto payload = makePayload(len);
    std::vector<std::vector<uint8_t>> frames;
    assert(buildFrames(kPuidB, 1, payload.data(), len, 1000, 7, frames));
    assert(frames.size() > 1);

    Reassembler reasm;
    std::vector<uint8_t> out;
    FrameHeader header{};
    const uint8_t* data = nullptr;
    size_t dataLen = 0;

    // Feed the first fragment twice: the second copy must be ignored.
    assert(parseFrame(frames[0].data(), frames[0].size(), header, data, dataLen));
    assert(!reasm.add(header, data, dataLen, 100, out));
    assert(!reasm.add(header, data, dataLen, 101, out));
    assert(reasm.pending() == 1);

    // Not complete yet -> gc must not drop it.
    reasm.gc(105, 10);
    assert(reasm.pending() == 1);

    // After the timeout it is dropped.
    reasm.gc(120, 10);
    assert(reasm.pending() == 0);
}

void testPendingCap() {
    Reassembler reasm;
    const std::vector<uint8_t> payload(10, 0);
    FrameHeader header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.type = kTypeData;
    header.fragCount = 2;
    header.fragIndex = 0;
    header.payloadLen = static_cast<uint16_t>(payload.size());
    std::memcpy(header.sender, kPuidA, kPuidLen);
    header.sender[kPuidLen] = '\0';

    std::vector<uint8_t> out;
    for (uint32_t i = 0; i < Reassembler::kMaxPending + 50; ++i) {
        header.msgId = i;
        reasm.add(header, payload.data(), payload.size(), 1000, out);
    }
    assert(reasm.pending() <= Reassembler::kMaxPending);
}

void testBuildRejectsBadInput() {
    std::vector<std::vector<uint8_t>> frames;
    uint8_t byte = 0;
    assert(!buildFrames("deadbeef", 0, &byte, 1, 1400, 1, frames));
    assert(!buildFrames(kPuidA, 0, nullptr, 1, 1400, 1, frames));
    assert(!buildFrames(kPuidA, 0, &byte, 1, sizeof(FrameHeader), 1, frames));
    std::vector<uint8_t> big(200 * 128, 0);
    assert(!buildFrames(kPuidA, 0, big.data(), big.size(), 200, 1, frames));  // too many fragments
}

}  // namespace

int main() {
    testFragmentation();
    testInvalidFrames();
    testReassemblerDedupAndTimeout();
    testBuildRejectsBadInput();
    testPendingCap();
    std::puts("protocol tests passed");
    return 0;
}
