#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace tof_net {

constexpr uint32_t kMagic = 0x31525441u; // "ATR1" little-endian: A ToF Relay v1
constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kNameLen = 64;
constexpr size_t kPathLen = 256;

enum class MessageType : uint16_t {
    Hello = 1,
    StartCapture = 2,
    StopCapture = 3,
    DataFrame = 4,
    Status = 5,
    Error = 6,
    Shutdown = 7,
    // Machine A sends current module calibration/configuration blobs after Hello
    // and before streaming RAW frames. Machine B persists these files and uses
    // them for TOFI instead of stale local files.
    CcbFile = 8,
    CfgFile = 9,
    // ADSD3500 ISP-specific: per-mode intrinsics + dealias parameters read
    // directly from hardware (cmd 0x01, 0x02). These are required by
    // InitTofiConfig_isp and cannot be extracted from the CCB file alone.
    DealiasData = 10
};

enum class Codec : uint16_t {
    None = 0,
    Zstd = 1
};

enum class StatusCode : uint16_t {
    Ok = 0,
    Connected = 1,
    WaitingForCommand = 2,
    Capturing = 3,
    Stopped = 4,
    Error = 5
};

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic = kMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = 0;
    uint32_t header_bytes = sizeof(MessageHeader);
    uint64_t payload_bytes = 0;
};

struct HelloPayload {
    char app_name[kNameLen] = {};
    char sdk_version[kNameLen] = {};
    uint32_t protocol_version = kProtocolVersion;
};

struct StartCapturePayload {
    char frame_type[kNameLen] = {};      // e.g. lr-qnative / sr-native, consumed by A.
    uint16_t mode = 0;                   // consumed by B TOFI config.
    uint16_t reserved = 0;
    uint32_t requested_fps = 0;          // 0 = unrestricted/device-driven.
};

struct StopCapturePayload {
    uint32_t reason = 0;
};

struct StatusPayload {
    uint16_t code = static_cast<uint16_t>(StatusCode::Ok);
    uint16_t reserved = 0;
    uint64_t frame_id = 0;
    char text[128] = {};
};

struct DataFramePayloadHeader {
    uint64_t frame_id = 0;
    uint64_t timestamp_ns = 0;
    uint32_t raw_width = 0;
    uint32_t raw_height = 0;
    uint32_t raw_bytes = 0;
    uint32_t compressed_bytes = 0;
    uint16_t codec = static_cast<uint16_t>(Codec::Zstd);
    uint16_t mode = 0;
    char frame_type[kNameLen] = {};
};

struct CcbFilePayloadHeader {
    uint32_t ccb_bytes = 0;
    uint32_t reserved = 0;
    char filename[kNameLen] = {}; // informational, e.g. module_<serial>.ccb
};

struct CfgFilePayloadHeader {
    uint32_t cfg_bytes = 0;
    uint32_t reserved = 0;
    char filename[kNameLen] = {}; // informational, e.g. module_<serial>.cfg
};

// Per-mode intrinsics + dealias data for InitTofiConfig_isp.
// Payload = header + data_bytes of raw TofiXYZDealiasData.
// Machine A sends one DealiasData message per available frame type.
struct DealiasDataPayload {
    char     frame_type[kNameLen] = {};  // e.g. "sr-native"
    uint32_t data_bytes = 0;            // sizeof(TofiXYZDealiasData)
    uint32_t reserved = 0;
    // Followed by data_bytes of raw TofiXYZDealiasData (CameraIntrinsics +
    // dealias params from adsd3500_read_payload_cmd 0x01 + 0x02).
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 20, "Unexpected MessageHeader layout");

inline void copy_cstr(char *dst, size_t dst_size, const std::string &src) {
    if (!dst || dst_size == 0) {
        return;
    }
    std::memset(dst, 0, dst_size);
    std::strncpy(dst, src.c_str(), dst_size - 1);
}

inline std::string cstr_to_string(const char *buf, size_t n) {
    if (!buf || n == 0) {
        return {};
    }
    size_t len = 0;
    while (len < n && buf[len] != '\0') {
        ++len;
    }
    return std::string(buf, len);
}

struct Message {
    MessageType type = MessageType::Status;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> pack_message(MessageType type, const void *payload, size_t payload_size);
Message unpack_message(const std::vector<uint8_t> &bytes);

} // namespace tof_net
