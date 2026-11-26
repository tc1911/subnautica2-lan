#include "protocol.h"

#include <algorithm>
#include <cstring>

namespace sn2lan {

namespace {

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string entryKey(const FrameHeader& header) {
    std::string key(header.sender, strnlen(header.sender, kPuidField));
    key.push_back('#');
    key += std::to_string(header.channel);
    key.push_back('#');
    key += std::to_string(header.msgId);
    return key;
}

}  // namespace

bool isValidPuid(const char* s) {
    if (s == nullptr) {
        return false;
    }
    size_t n = 0;
    while (n < kPuidLen && s[n] != '\0') {
        if (!isHex(s[n])) {
            return false;
        }
        ++n;
    }
    return n == kPuidLen && s[kPuidLen] == '\0';
}

bool buildFrames(const std::string& senderPuid,
                 uint8_t channel,
                 const void* data,
                 size_t len,
                 size_t mtu,
                 uint32_t msgId,
                 std::vector<std::vector<uint8_t>>& out) {
    out.clear();
    if (senderPuid.size() != kPuidLen || !isValidPuid(senderPuid.c_str())) {
        return false;
    }
    if (len > 0 && data == nullptr) {
        return false;
    }
    if (mtu <= sizeof(FrameHeader) + 16 || mtu > 60000) {
        return false;
    }

    const size_t chunk = mtu - sizeof(FrameHeader);
    const size_t fragCount = len == 0 ? 1 : (len + chunk - 1) / chunk;
    if (fragCount > kMaxFragments) {
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(data);
    out.reserve(fragCount);
    for (size_t i = 0; i < fragCount; ++i) {
        const size_t offset = i * chunk;
        const size_t partLen = std::min(chunk, len - offset);

        FrameHeader header{};
        header.magic = kMagic;
        header.version = kVersion;
        header.type = kTypeData;
        header.channel = channel;
        header.flags = 0;
        header.msgId = msgId;
        header.fragIndex = static_cast<uint16_t>(i);
        header.fragCount = static_cast<uint16_t>(fragCount);
        std::memcpy(header.sender, senderPuid.data(), kPuidLen);
        header.sender[kPuidLen] = '\0';
        header.payloadLen = static_cast<uint16_t>(partLen);

        std::vector<uint8_t> frame(sizeof(FrameHeader) + partLen);
        std::memcpy(frame.data(), &header, sizeof(FrameHeader));
        if (partLen > 0) {
            std::memcpy(frame.data() + sizeof(FrameHeader), bytes + offset, partLen);
        }
        out.push_back(std::move(frame));
    }
    return true;
}

bool parseFrame(const uint8_t* buf,
                size_t size,
                FrameHeader& header,
                const uint8_t*& payload,
                size_t& payloadLen) {
    if (buf == nullptr || size < sizeof(FrameHeader)) {
        return false;
    }
    std::memcpy(&header, buf, sizeof(FrameHeader));

    if (header.magic != kMagic || header.version != kVersion) {
        return false;
    }
    if (header.type != kTypeData && header.type != kTypeHello) {
        return false;
    }
    if ((header.flags & ~kFlagHostReady) != 0) {
        return false;
    }
    if (header.fragCount == 0 || header.fragCount > kMaxFragments || header.fragIndex >= header.fragCount) {
        return false;
    }
    if (header.sender[kPuidLen] != '\0' || !isValidPuid(header.sender)) {
        return false;
    }
    const size_t remaining = size - sizeof(FrameHeader);
    if (header.payloadLen != remaining) {
        return false;
    }
    if (header.type == kTypeHello && header.payloadLen != 0) {
        return false;
    }

    payload = buf + sizeof(FrameHeader);
    payloadLen = header.payloadLen;
    return true;
}

bool Reassembler::add(const FrameHeader& header,
                      const uint8_t* payload,
                      size_t payloadLen,
                      uint64_t nowMs,
                      std::vector<uint8_t>& out) {
    if (header.fragCount == 1) {
        out.assign(payload, payload + payloadLen);
        return true;
    }

    std::string key = entryKey(header);
    auto existing = entries_.find(key);
    if (existing == entries_.end() && entries_.size() >= kMaxPending) {
        gc(nowMs);
        if (entries_.size() >= kMaxPending) {
            return false;
        }
    }
    Entry& entry = entries_[key];
    if (entry.parts.empty()) {
        entry.count = header.fragCount;
        entry.channel = header.channel;
        entry.parts.resize(header.fragCount);
        entry.have.assign(header.fragCount, 0);
    } else if (entry.count != header.fragCount) {
        entries_.erase(key);
        return false;
    }

    if (entry.have[header.fragIndex] != 0) {
        entry.lastMs = nowMs;
        return false;
    }

    entry.parts[header.fragIndex].assign(payload, payload + payloadLen);
    entry.have[header.fragIndex] = 1;
    entry.got = static_cast<uint16_t>(entry.got + 1);
    entry.total += payloadLen;
    entry.lastMs = nowMs;

    if (entry.got < entry.count) {
        return false;
    }

    out.clear();
    out.reserve(entry.total);
    for (const auto& part : entry.parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    entries_.erase(key);
    return true;
}

void Reassembler::gc(uint64_t nowMs, uint64_t timeoutMs) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (nowMs > it->second.lastMs && nowMs - it->second.lastMs > timeoutMs) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void Reassembler::clear() {
    entries_.clear();
}

}  // namespace sn2lan
