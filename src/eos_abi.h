// Minimal ABI declarations for the small part of the EOS SDK that we touch.
//
// We deliberately do not depend on the EOS SDK headers or on its import
// library: the game ships EOSSDK-Win64-Shipping.dll and loads it lazily, so we
// resolve the exports at runtime and call/patch them by address. The layouts
// below match EOS SDK 1.18 (see docs.rs/eos-sys and the official API reference).
#pragma once

#include <cstdint>

namespace sn2lan {

using EOS_EResult = int32_t;
struct EOS_ProductUserIdDetails;
struct EOS_HP2PDetails;
using EOS_ProductUserId = EOS_ProductUserIdDetails*;
using EOS_HP2P = EOS_HP2PDetails*;

constexpr EOS_EResult kEosSuccess = 0;

struct EOS_P2P_SocketId {
    int32_t ApiVersion;
    char SocketName[33];
};

struct EOS_P2P_SendPacketOptions {
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId RemoteUserId;
    const EOS_P2P_SocketId* SocketId;
    uint8_t Channel;
    uint32_t DataLengthBytes;
    const void* Data;
    int32_t bAllowDelayedDelivery;
    int32_t Reliability;
    int32_t bDisableAutoAcceptConnection;
};

struct EOS_P2P_ReceivePacketOptions {
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    uint32_t MaxDataSizeBytes;
    const uint8_t* RequestedChannel;
};

struct EOS_P2P_GetNextReceivedPacketSizeOptions {
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    const uint8_t* RequestedChannel;
};

static_assert(sizeof(EOS_P2P_SocketId) == 40, "EOS_P2P_SocketId layout changed");
static_assert(sizeof(EOS_P2P_SendPacketOptions) == 64, "EOS_P2P_SendPacketOptions layout changed");
static_assert(sizeof(EOS_P2P_ReceivePacketOptions) == 32, "EOS_P2P_ReceivePacketOptions layout changed");
static_assert(sizeof(EOS_P2P_GetNextReceivedPacketSizeOptions) == 24, "EOS_P2P_GetNextReceivedPacketSizeOptions layout changed");

using PFN_EOS_P2P_SendPacket = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_SendPacketOptions*);
using PFN_EOS_P2P_ReceivePacket = EOS_EResult (*)(EOS_HP2P,
                                                  const EOS_P2P_ReceivePacketOptions*,
                                                  EOS_ProductUserId*,
                                                  EOS_P2P_SocketId*,
                                                  uint8_t*,
                                                  void*,
                                                  uint32_t*);
using PFN_EOS_P2P_GetNextReceivedPacketSize = EOS_EResult (*)(EOS_HP2P,
                                                              const EOS_P2P_GetNextReceivedPacketSizeOptions*,
                                                              uint32_t*);
using PFN_EOS_ProductUserId_ToString = EOS_EResult (*)(EOS_ProductUserId, char*, int32_t*);
using PFN_EOS_ProductUserId_FromString = EOS_ProductUserId (*)(const char*);

struct EosApi {
    PFN_EOS_P2P_SendPacket sendPacket = nullptr;
    PFN_EOS_P2P_ReceivePacket receivePacket = nullptr;
    PFN_EOS_P2P_GetNextReceivedPacketSize getNextReceivedPacketSize = nullptr;
    PFN_EOS_ProductUserId_ToString puidToString = nullptr;
    PFN_EOS_ProductUserId_FromString puidFromString = nullptr;
};

}  // namespace sn2lan
