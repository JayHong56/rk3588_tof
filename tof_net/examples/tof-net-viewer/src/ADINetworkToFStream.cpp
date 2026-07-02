/********************************************************************************/
/*                                                                              */
/* Copyright (c) 2020 Analog Devices, Inc. All Rights Reserved.                 */
/* Modified for network-only RAW ToF viewing.                                   */
/*                                                                              */
/********************************************************************************/
#include "ADINetworkToFStream.h"

#include "tof_net/zstd_codec.hpp"

#include <algorithm>
#include <cerrno>
#include <cfenv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>

#ifdef USE_GLOG
#include <glog/logging.h>
#else
#include <aditof/log.h>
#endif

#if defined(__has_include)
#if __has_include(<tofi/tofi_compute.h>)
#include <tofi/tofi_compute.h>
#elif __has_include(<tofi_compute.h>)
#include <tofi_compute.h>
#elif __has_include("tofi_compute.h")
#include "tofi_compute.h"
#else
#error "Cannot find TOFI compute header. Add the directory containing tofi_compute.h to the tof-net-viewer include path."
#endif
#if __has_include(<tofi/tofi_config.h>)
#include <tofi/tofi_config.h>
#elif __has_include(<tofi_config.h>)
#include <tofi_config.h>
#elif __has_include("tofi_config.h")
#include "tofi_config.h"
#else
#error "Cannot find TOFI config header. Add the directory containing tofi_config.h to the tof-net-viewer include path."
#endif
#else
#include <tofi_compute.h>
#include <tofi_config.h>
#endif

using namespace adicontroller;

#if defined(__GLIBC__) && !defined(__APPLE__)
extern "C" int fedisableexcept(int excepts) __attribute__((weak));
#endif

namespace {

void maskFloatingPointTrapsForTofi() {
#if defined(__GLIBC__) && !defined(__APPLE__)
    if (fedisableexcept != nullptr) {
        fedisableexcept(FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW);
    }
#endif
    std::feclearexcept(FE_ALL_EXCEPT);
}

// ---------------------------------------------------------------------------
// ADSD3500 mode name → TOFI mode ID mapping.
// Mirrors the SDK's ModeInfo::g_newModesAdsd3500 table and
// ModeInfo::convertCameraMode(). Used by ensureTofiIsp() to convert
// the frame_type string (e.g. "lr-qnative") to the uint8_t mode ID
// expected by InitTofiConfig_isp().
// ---------------------------------------------------------------------------
struct Adsd3500ModeEntry {
    uint8_t     modeId;
    const char *modeName;
};

static const Adsd3500ModeEntry kAdsd3500ModeTable[] = {
    {0, "sr-native"},
    {1, "lr-native"},
    {2, "sr-qnative"},
    {3, "lr-qnative"},
    {4, "pcm-native"},
    {5, "lr-mixed"},
    {6, "sr-mixed"},
};

std::string trimFrameType(std::string frameType) {
    frameType.erase(frameType.begin(),
                    std::find_if(frameType.begin(), frameType.end(),
                                 [](unsigned char c) { return !std::isspace(c); }));
    frameType.erase(std::find_if(frameType.rbegin(), frameType.rend(),
                                 [](unsigned char c) { return !std::isspace(c); })
                        .base(),
                    frameType.end());
    return frameType;
}

std::string sanitizeFrameType(std::string frameType) {
    frameType = trimFrameType(std::move(frameType));
    if (frameType.empty()) {
        return "lr-mixed";
    }
#ifndef ENBABLE_PASSIVE_IR
    if (frameType == "pcm" || frameType == "pcm-native") {
        LOG(WARNING) << "Passive IR frame type " << frameType
                     << " requested in non-passive-IR build; using lr-mixed";
        return "lr-mixed";
    }
#endif
    return frameType;
}

} // namespace

uint8_t adicontroller::ADINetworkToFStream::convertModeName(
    const std::string &frameType) {
    // Strip whitespace and try exact match first, then prefix match.
    std::string ft = trimFrameType(frameType);

    if (ft.empty()) {
        return 0;
    }

    for (const auto &entry : kAdsd3500ModeTable) {
        if (ft == entry.modeName) {
            return entry.modeId;
        }
    }
    // Fallback: prefix match (handles "lr-qnative" variants).
    for (const auto &entry : kAdsd3500ModeTable) {
        if (ft.find(entry.modeName) != std::string::npos) {
            return entry.modeId;
        }
    }
    return 0;
}

namespace {

void validatePlaneSize(uint32_t width, uint32_t height,
                       uint32_t subelementSize,
                       uint32_t subelementsPerElement) {
    const uint64_t bytes = static_cast<uint64_t>(width) * height *
                           subelementSize * subelementsPerElement;
    if (bytes > UINT32_MAX) {
        throw std::runtime_error("Computed frame plane is too large");
    }
}

void addPlane(std::vector<aditof::FrameDataDetails> &planes,
              const std::string &type, uint32_t width, uint32_t height,
              uint32_t subelementSize, uint32_t subelementsPerElement) {
    aditof::FrameDataDetails details;
    details.type = type;
    details.width = width;
    details.height = height;
    details.subelementSize = subelementSize;
    details.subelementsPerElement = subelementsPerElement;
    validatePlaneSize(width, height, subelementSize, subelementsPerElement);
    planes.push_back(details);
}

std::string makeFrameType(const tof_net::DataFramePayloadHeader &header,
                          const ADINetworkToFStream::Config &config) {
    std::string frameType =
        tof_net::cstr_to_string(header.frame_type, tof_net::kNameLen);
    if (frameType.empty()) {
        frameType = config.frameType;
    }
    if (frameType.empty()) {
        frameType = "network-raw";
    }
    return frameType;
}

uint16_t makeMode(const tof_net::DataFramePayloadHeader &header,
                  const ADINetworkToFStream::Config &config) {
    return header.mode != 0 ? header.mode : config.mode;
}

bool fileReadable(const std::string &path) {
    if (path.empty()) {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    return stream.good();
}

std::vector<std::string> defaultIniCandidatesForFrameType(
    const std::string &frameType) {
    std::vector<std::string> candidates;
    if (!frameType.empty()) {
        candidates.push_back("./config/RawToDepthAdsd3500_" + frameType + ".ini");
        candidates.push_back("./config/RawToDepthAdsd_" + frameType + ".ini");
        candidates.push_back("../tof-net-collect/config/RawToDepthAdsd3500_" +
                             frameType + ".ini");
        candidates.push_back("../tof-net-collect/config/RawToDepthAdsd_" +
                             frameType + ".ini");
        candidates.push_back("../../tof-net-collect/config/RawToDepthAdsd3500_" +
                             frameType + ".ini");
        candidates.push_back("../../tof-net-collect/config/RawToDepthAdsd_" +
                             frameType + ".ini");
    }
    return candidates;
}

std::string resolveIniFile(const std::string &iniFile,
                           const std::string &frameType) {
    if (!iniFile.empty()) {
        return iniFile;
    }

    const std::vector<std::string> candidates =
        defaultIniCandidatesForFrameType(frameType);
    for (const std::string &candidate : candidates) {
        if (fileReadable(candidate)) {
            return candidate;
        }
    }

    if (!candidates.empty()) {
        return candidates.front();
    }
    return iniFile;
}

std::string parentDirOf(const std::string &path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return {};
    }
    if (pos == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, pos);
}

void ensureDirectoryTree(const std::string &dir) {
    if (dir.empty() || dir == ".") {
        return;
    }

    std::string current;
    size_t start = 0;
    if (!dir.empty() && dir[0] == '/') {
        current = "/";
        start = 1;
    }

    while (start <= dir.size()) {
        const size_t pos = dir.find('/', start);
        const std::string part = dir.substr(start, pos == std::string::npos ?
                                                    std::string::npos : pos - start);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') {
                current += '/';
            }
            current += part;
            if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                throw std::runtime_error("mkdir failed for " + current + ": " +
                                         std::strerror(errno));
            }
        }
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 1;
    }
}

constexpr auto kSocketPollInterval = std::chrono::milliseconds(1000);
constexpr auto kActiveStreamSilenceTimeout = std::chrono::seconds(5);
constexpr auto kMinTofiFrameInterval = std::chrono::milliseconds(250);

bool waitForReadableSocket(int fd, std::chrono::milliseconds timeout,
                           short *revents) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLERR | POLLHUP | POLLNVAL;

    int rc = 0;
    do {
        rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        throw std::runtime_error(std::string("poll failed: ") +
                                 std::strerror(errno));
    }

    if (revents) {
        *revents = pfd.revents;
    }
    return rc > 0;
}

std::string socketEventsToString(short events) {
    std::ostringstream ss;
    bool first = true;
    auto append = [&](const char *name) {
        if (!first) {
            ss << "|";
        }
        ss << name;
        first = false;
    };

    if (events & POLLIN) append("POLLIN");
    if (events & POLLERR) append("POLLERR");
    if (events & POLLHUP) append("POLLHUP");
    if (events & POLLNVAL) append("POLLNVAL");
    if (first) ss << "0";
    return ss.str();
}

} // namespace

ADINetworkToFStream::ADINetworkToFStream()
    : m_stopFlag(false), m_running(false), m_clientConnected(false),
      m_remoteCapturing(false), m_waitingForFirstFrame(false),
      m_autoStartSent(false), m_lastStartCommandTime(std::chrono::steady_clock::now()),
      m_lastTofiFrameTime(std::chrono::steady_clock::time_point::min()),
      m_tofiConfig(nullptr),
      m_tofiContext(nullptr), m_tofiMode(0) {
    maskFloatingPointTrapsForTofi();
    setStatus("Network listener is idle");
}

ADINetworkToFStream::~ADINetworkToFStream() {
    stop();
    releaseTofi();
}

void ADINetworkToFStream::configure(const Config &config) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config = config;
    m_config.frameType = sanitizeFrameType(m_config.frameType);
    m_config.mode = convertModeName(m_config.frameType);
}

void ADINetworkToFStream::start() {
    if (m_running.load()) {
        return;
    }

    m_stopFlag = false;
    m_workerThread = std::thread(&ADINetworkToFStream::workerLoop, this);
}

void ADINetworkToFStream::stop() {
    m_stopFlag = true;
    try {
        sendStopCaptureCommand();
    } catch (...) {
    }

    {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        m_listenSocket.close();
        m_clientSocket.close();
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_queue.erase();
    m_running = false;
    m_clientConnected = false;
    m_remoteCapturing = false;
    m_waitingForFirstFrame = false;
    m_autoStartSent = false;
    setStatus("Network listener stopped");
}

void ADINetworkToFStream::requestFrame() {
    // The remote producer drives frame cadence. This method is intentionally a
    // no-op so the existing viewer display loop can keep its requestFrame call.
}

void ADINetworkToFStream::setFrameType(const std::string &frameType) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config.frameType = sanitizeFrameType(frameType);
    m_config.mode = convertModeName(m_config.frameType);
}

std::shared_ptr<aditof::Frame> ADINetworkToFStream::getFrame() {
    std::shared_ptr<aditof::Frame> frame;
    if (m_queue.dequeue_for(frame, std::chrono::milliseconds(500)) && frame) {
        m_lastFrame = frame;
        return frame;
    }

    if (m_lastFrame) {
        return m_lastFrame;
    }

    if (m_waitingForFirstFrame.load()) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m_lastStartCommandTime)
                .count();
        std::ostringstream ss;
        ss << "Waiting " << elapsed
           << "s for first RAW ToF frame after StartCapture";
        if (!m_clientConnected.load()) {
            ss << "; Machine A disconnected, likely camera side restarted";
        }
        setStatus(ss.str());
    } else {
        setStatus("Waiting for first RAW ToF frame from network client");
    }
    m_lastFrame = buildEmptyFrame();
    return m_lastFrame;
}

bool ADINetworkToFStream::isRunning() const { return m_running.load(); }

bool ADINetworkToFStream::hasClient() const {
    return m_clientConnected.load();
}

std::string ADINetworkToFStream::statusText() const {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_statusText;
}

bool ADINetworkToFStream::startRemoteCapture() {
    return sendStartCaptureCommand();
}

bool ADINetworkToFStream::stopRemoteCapture() {
    return sendStopCaptureCommand();
}

bool ADINetworkToFStream::sendStartCaptureCommand() {
    Config config;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        config = m_config;
    }

    tof_net::StartCapturePayload payload;
    tof_net::copy_cstr(payload.frame_type, sizeof(payload.frame_type),
                       config.frameType);
    payload.mode = convertModeName(config.frameType);
    payload.requested_fps = config.requestedFps;

    try {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (!m_clientSocket.valid()) {
            setStatus("Cannot start capture: no tof_net_collect client is connected");
            return false;
        }
        m_clientSocket.send_message(tof_net::MessageType::StartCapture, &payload,
                                    sizeof(payload));
    } catch (const std::exception &e) {
        setStatus(std::string("Failed to send StartCapture: ") + e.what());
        return false;
    }

    m_remoteCapturing = true;
    m_waitingForFirstFrame = true;
    m_lastStartCommandTime = std::chrono::steady_clock::now();
    m_lastTofiFrameTime = std::chrono::steady_clock::time_point::min();
    std::ostringstream ss;
    ss << "Sent StartCapture to Machine A"
       << " mode=" << payload.mode;
    if (!config.frameType.empty()) {
        ss << " frame_type=" << config.frameType;
    }
    ss << " requested_fps=" << payload.requested_fps;
    setStatus(ss.str());
    LOG(INFO) << ss.str();
    return true;
}

bool ADINetworkToFStream::sendStopCaptureCommand() {
    try {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (!m_clientSocket.valid()) {
            return false;
        }

        tof_net::StopCapturePayload payload;
        payload.reason = 0;
        m_clientSocket.send_message(tof_net::MessageType::StopCapture, &payload,
                                    sizeof(payload));
    } catch (const std::exception &e) {
        setStatus(std::string("Failed to send StopCapture: ") + e.what());
        return false;
    }

    m_remoteCapturing = false;
    m_waitingForFirstFrame = false;
    setStatus("Sent StopCapture to Machine A");
    return true;
}

void ADINetworkToFStream::workerLoop() {
    Config config;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        config = m_config;
    }

    m_running = true;

    while (!m_stopFlag.load()) {
        try {
            m_listenSocket.close();
            m_listenSocket.bind_listen(config.listenIp, config.listenPort, 1);

            std::ostringstream ss;
            ss << "Listening for RAW ToF frames on " << config.listenIp << ":"
               << config.listenPort;
            setStatus(ss.str());

            while (!m_stopFlag.load()) {
                std::string peerIp;
                uint16_t peerPort = 0;
                tof_net::Socket accepted =
                    m_listenSocket.accept_one(&peerIp, &peerPort);
                {
                    std::lock_guard<std::mutex> lock(m_socketMutex);
                    m_clientSocket.close();
                    m_clientSocket = std::move(accepted);
                    m_clientConnected = true;
                    m_remoteCapturing = false;
                    m_autoStartSent = false;
                }

                std::ostringstream connected;
                connected << "RAW ToF client connected from " << peerIp << ":"
                          << peerPort;
                setStatus(connected.str());
                LOG(INFO) << connected.str();

                // Do not start capture here. Open Device only establishes the
                // TCP connection. The Play button explicitly sends
                // StartCapture so Machine A begins streaming only when the user
                // requests display.

                auto lastMessageTime = std::chrono::steady_clock::now();
                while (!m_stopFlag.load() && m_clientSocket.valid()) {
                    short socketEvents = 0;
                    const bool readable = waitForReadableSocket(
                        m_clientSocket.fd(), kSocketPollInterval,
                        &socketEvents);
                    if (!readable) {
                        const bool activeStream =
                            m_remoteCapturing.load() ||
                            m_waitingForFirstFrame.load();
                        const auto silence =
                            std::chrono::steady_clock::now() -
                            lastMessageTime;
                        if (activeStream &&
                            silence >= kActiveStreamSilenceTimeout) {
                            std::ostringstream stale;
                            stale << "RAW ToF client timed out during capture; "
                                  << "no network message for "
                                  << std::chrono::duration_cast<
                                         std::chrono::seconds>(silence)
                                         .count()
                                  << "s, closing stale socket and waiting for "
                                     "reconnect";
                            setStatus(stale.str());
                            LOG(WARNING) << stale.str();
                            break;
                        }
                        continue;
                    }

                    if ((socketEvents & (POLLERR | POLLNVAL)) ||
                        ((socketEvents & POLLHUP) &&
                         !(socketEvents & POLLIN))) {
                        std::ostringstream gone;
                        gone << "RAW ToF client socket closed/events="
                             << socketEventsToString(socketEvents);
                        setStatus(gone.str());
                        LOG(WARNING) << gone.str();
                        break;
                    }

                    tof_net::Message message;
                    try {
                        message = m_clientSocket.recv_message();
                    } catch (const std::exception &e) {
                        std::ostringstream gone;
                        gone << "RAW ToF client disconnected while receiving: "
                             << e.what();
                        setStatus(gone.str());
                        LOG(WARNING) << gone.str();
                        break;
                    }
                    lastMessageTime = std::chrono::steady_clock::now();
                    if (message.type == tof_net::MessageType::DataFrame) {
                        handleDataFrame(message);
                    } else if (message.type == tof_net::MessageType::Hello) {
                        if (message.payload.size() >= sizeof(tof_net::HelloPayload)) {
                            tof_net::HelloPayload hello;
                            std::memcpy(&hello, message.payload.data(), sizeof(hello));
                            std::ostringstream hs;
                            hs << "Remote hello: "
                               << tof_net::cstr_to_string(hello.app_name, tof_net::kNameLen)
                               << ", SDK "
                               << tof_net::cstr_to_string(hello.sdk_version, tof_net::kNameLen);
                            setStatus(hs.str());
                            LOG(INFO) << hs.str();
                        }
                    } else if (message.type == tof_net::MessageType::CcbFile) {
                        handleCcbFile(message);
                    } else if (message.type == tof_net::MessageType::CfgFile) {
                        handleCfgFile(message);
                    } else if (message.type == tof_net::MessageType::DealiasData) {
                        handleDealiasData(message);
                    } else if (message.type == tof_net::MessageType::Status) {
                        if (message.payload.size() >= sizeof(tof_net::StatusPayload)) {
                            tof_net::StatusPayload st;
                            std::memcpy(&st, message.payload.data(), sizeof(st));
                            std::ostringstream rs;
                            rs << "Remote status code=" << st.code << ": "
                               << tof_net::cstr_to_string(st.text, sizeof(st.text))
                               << " frame=" << st.frame_id;
                            setStatus(rs.str());
                            LOG(INFO) << rs.str();
                            const std::string statusText =
                                tof_net::cstr_to_string(st.text, sizeof(st.text));
                            if (st.code == static_cast<uint16_t>(
                                               tof_net::StatusCode::Stopped)) {
                                m_remoteCapturing = false;
                                m_waitingForFirstFrame = false;
                            }
                            if (st.code == static_cast<uint16_t>(
                                               tof_net::StatusCode::WaitingForCommand) &&
                                statusText.find("waiting for start command") !=
                                    std::string::npos) {
                                m_autoStartSent = false;
                            }
                        } else {
                            setStatus("Remote status message received");
                        }
                    } else if (message.type == tof_net::MessageType::Error) {
                        setStatus("Remote producer reported an error");
                    } else if (message.type == tof_net::MessageType::Shutdown) {
                        setStatus("Remote producer disconnected");
                        break;
                    }
                }

                const bool captureWasActive = m_remoteCapturing.load();
                const bool waitingForFirstFrame =
                    m_waitingForFirstFrame.load();
                {
                    std::lock_guard<std::mutex> lock(m_socketMutex);
                    m_clientSocket.close();
                    m_clientConnected = false;
                    m_remoteCapturing = false;
                    m_waitingForFirstFrame = false;
                }
                if (!m_stopFlag.load()) {
                    if (captureWasActive && waitingForFirstFrame) {
                        setStatus("RAW ToF client disconnected before first frame after StartCapture; Machine A or ToF camera likely restarted");
                    } else if (captureWasActive) {
                        setStatus("RAW ToF client disconnected during capture; waiting for reconnect");
                    } else {
                        setStatus("RAW ToF client disconnected; waiting for reconnect");
                    }
                }
            }
        } catch (const std::exception &e) {
            if (!m_stopFlag.load()) {
                std::ostringstream ss;
                ss << "Network listener error: " << e.what();
                setStatus(ss.str());
                LOG(ERROR) << ss.str();
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                m_clientSocket.close();
                m_listenSocket.close();
                m_clientConnected = false;
                m_remoteCapturing = false;
                m_waitingForFirstFrame = false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        m_clientSocket.close();
        m_listenSocket.close();
    }
    m_clientConnected = false;
    m_remoteCapturing = false;
    m_waitingForFirstFrame = false;
    m_running = false;
}

void ADINetworkToFStream::handleCcbFile(const tof_net::Message &message) {
    if (message.payload.size() < sizeof(tof_net::CcbFilePayloadHeader)) {
        throw std::runtime_error("Malformed CcbFile message: header missing");
    }

    tof_net::CcbFilePayloadHeader header;
    std::memcpy(&header, message.payload.data(), sizeof(header));

    const size_t payloadDataOffset = sizeof(header);
    const size_t availablePayloadBytes = message.payload.size() - payloadDataOffset;
    if (header.ccb_bytes == 0) {
        throw std::runtime_error("Received empty module CCB from Machine A");
    }
    if (header.ccb_bytes > availablePayloadBytes) {
        throw std::runtime_error("Malformed CcbFile message: payload truncated");
    }

    std::string savePath;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        savePath = m_config.receivedCcbFile.empty()
                       ? std::string("./config/received_machine_a_module.ccb")
                       : m_config.receivedCcbFile;
    }

    ensureDirectoryTree(parentDirOf(savePath));

    const std::string tmpPath = savePath + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Cannot create received CCB file: " + tmpPath);
        }
        out.write(reinterpret_cast<const char *>(message.payload.data() + payloadDataOffset),
                  header.ccb_bytes);
        if (!out) {
            throw std::runtime_error("Failed while writing received CCB file: " + tmpPath);
        }
    }

    if (std::rename(tmpPath.c_str(), savePath.c_str()) != 0) {
        throw std::runtime_error("rename failed for received CCB file: " +
                                 std::string(std::strerror(errno)));
    }

    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.ccbFile = savePath;
    }

    // The calibration changed. Drop any existing TOFI context so the next RAW
    // frame is computed using Machine A's current module CCB.
    {
        std::lock_guard<std::mutex> lock(m_tofiMutex);
        releaseTofi();
    }
    m_receivedCcbFile = savePath;

    std::ostringstream ss;
    ss << "Received current module CCB from Machine A: " << header.ccb_bytes
       << " bytes, source="
       << tof_net::cstr_to_string(header.filename, tof_net::kNameLen)
       << ", saved=" << savePath
       << "; TOFI will use received CCB";
    setStatus(ss.str());
    LOG(INFO) << ss.str();
}

void ADINetworkToFStream::handleCfgFile(const tof_net::Message &message) {
    if (message.payload.size() < sizeof(tof_net::CfgFilePayloadHeader)) {
        throw std::runtime_error("Malformed CfgFile message: header missing");
    }

    tof_net::CfgFilePayloadHeader header;
    std::memcpy(&header, message.payload.data(), sizeof(header));

    const size_t payloadDataOffset = sizeof(header);
    const size_t availablePayloadBytes = message.payload.size() - payloadDataOffset;
    if (header.cfg_bytes == 0) {
        throw std::runtime_error("Received empty module CFG from Machine A");
    }
    if (header.cfg_bytes > availablePayloadBytes) {
        throw std::runtime_error("Malformed CfgFile message: payload truncated");
    }

    std::string savePath;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        savePath = m_config.receivedCfgFile.empty()
                       ? std::string("./config/received_machine_a_module.cfg")
                       : m_config.receivedCfgFile;
    }

    ensureDirectoryTree(parentDirOf(savePath));

    const std::string tmpPath = savePath + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Cannot create received CFG file: " + tmpPath);
        }
        out.write(reinterpret_cast<const char *>(message.payload.data() + payloadDataOffset),
                  header.cfg_bytes);
        if (!out) {
            throw std::runtime_error("Failed while writing received CFG file: " + tmpPath);
        }
    }

    if (std::rename(tmpPath.c_str(), savePath.c_str()) != 0) {
        throw std::runtime_error("rename failed for received CFG file: " +
                                 std::string(std::strerror(errno)));
    }

    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.cfgFile = savePath;
    }

    // The module configuration changed. Drop any existing TOFI context so the
    // next RAW frame is computed using Machine A's current module CFG.
    {
        std::lock_guard<std::mutex> lock(m_tofiMutex);
        releaseTofi();
    }
    m_receivedCfgFile = savePath;

    std::ostringstream ss;
    ss << "Received current module CFG from Machine A: " << header.cfg_bytes
       << " bytes, source="
       << tof_net::cstr_to_string(header.filename, tof_net::kNameLen)
       << ", saved=" << savePath
       << "; TOFI will use received CFG";
    setStatus(ss.str());
    LOG(INFO) << ss.str();
}

void ADINetworkToFStream::handleDealiasData(
    const tof_net::Message &message) {
    if (message.payload.size() < sizeof(tof_net::DealiasDataPayload)) {
        throw std::runtime_error(
            "Malformed DealiasData message: header missing");
    }

    tof_net::DealiasDataPayload header;
    std::memcpy(&header, message.payload.data(), sizeof(header));

    const size_t dataOffset = sizeof(header);
    const size_t available = message.payload.size() - dataOffset;

    if (header.data_bytes == 0 || header.data_bytes > available) {
        throw std::runtime_error(
            "Malformed DealiasData message: payload size mismatch");
    }
    if (header.data_bytes != sizeof(TofiXYZDealiasData)) {
        std::ostringstream warn;
        warn << "DealiasData size mismatch: received " << header.data_bytes
             << " bytes, expected " << sizeof(TofiXYZDealiasData)
             << "; using anyway";
        setStatus(warn.str());
        LOG(WARNING) << warn.str();
    }

    const std::string frameType =
        tof_net::cstr_to_string(header.frame_type, tof_net::kNameLen);
    if (frameType.empty()) {
        throw std::runtime_error(
            "Malformed DealiasData message: empty frame_type");
    }

    const uint8_t modeId = convertModeName(frameType);

    TofiXYZDealiasData dealias;
    std::memset(&dealias, 0, sizeof(dealias));
    std::memcpy(&dealias,
                message.payload.data() + dataOffset,
                std::min(header.data_bytes,
                         static_cast<uint32_t>(sizeof(dealias))));

    {
        std::lock_guard<std::mutex> lock(m_tofiMutex);
        m_xyzDealiasData[modeId] = dealias;
        releaseTofi();
    }

    std::cout << "[viewer] DealiasData received: " << frameType
              << " modeId=" << static_cast<int>(modeId)
              << " rows=" << dealias.n_rows
              << " cols=" << dealias.n_cols
              << " freqs=" << static_cast<int>(dealias.n_freqs)
              << " sensor=" << dealias.n_sensor_rows
              << "x" << dealias.n_sensor_cols
              << " fx=" << dealias.camera_intrinsics.fx
              << " fy=" << dealias.camera_intrinsics.fy
              << std::endl;

    std::ostringstream ss;
    ss << "Received dealias data for " << frameType
       << " (modeId=" << static_cast<int>(modeId)
       << ", rows=" << dealias.n_rows << ", cols=" << dealias.n_cols
       << ", " << header.data_bytes << " bytes)";
    setStatus(ss.str());
    LOG(INFO) << ss.str();
}

void ADINetworkToFStream::handleDataFrame(const tof_net::Message &message) {
    if (message.payload.size() < sizeof(tof_net::DataFramePayloadHeader)) {
        throw std::runtime_error("Malformed DataFrame message: header missing");
    }

    tof_net::DataFramePayloadHeader header;
    std::memcpy(&header, message.payload.data(), sizeof(header));

    const size_t payloadDataOffset = sizeof(header);
    size_t compressedBytes = header.compressed_bytes;
    const size_t availablePayloadBytes = message.payload.size() - payloadDataOffset;
    if (compressedBytes == 0) {
        compressedBytes = availablePayloadBytes;
    }
    if (compressedBytes > availablePayloadBytes) {
        throw std::runtime_error("Malformed DataFrame message: payload truncated");
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_lastTofiFrameTime != std::chrono::steady_clock::time_point::min() &&
        now - m_lastTofiFrameTime < kMinTofiFrameInterval) {
        std::ostringstream skipped;
        skipped << "Dropped RAW frame " << header.frame_id
                << "; viewer TOFI backpressure";
        setStatus(skipped.str());
        return;
    }
    m_lastTofiFrameTime = now;

    const uint8_t *payloadData = message.payload.data() + payloadDataOffset;
    std::vector<uint8_t> rawBytes;

    if (header.codec == static_cast<uint16_t>(tof_net::Codec::None)) {
        rawBytes.assign(payloadData, payloadData + compressedBytes);
    } else if (header.codec == static_cast<uint16_t>(tof_net::Codec::Zstd)) {
        rawBytes = tof_net::zstd_decompress(payloadData, compressedBytes,
                                            header.raw_bytes);
    } else {
        throw std::runtime_error("Unsupported RAW frame codec");
    }

    if (rawBytes.size() != header.raw_bytes) {
        throw std::runtime_error("Decoded RAW frame size does not match header");
    }

    if (header.frame_id == 1) {
        std::ostringstream ss;
        ss << "First RAW frame header: " << header.raw_width << "x"
           << header.raw_height << ", raw_bytes=" << header.raw_bytes
           << ", codec=" << header.codec << ", mode=" << header.mode
           << ", frame_type="
           << tof_net::cstr_to_string(header.frame_type, tof_net::kNameLen);
        setStatus(ss.str());
    }
    if (m_waitingForFirstFrame.exchange(false)) {
        std::ostringstream ss;
        ss << "First RAW frame arrived after StartCapture: frame="
           << header.frame_id << " " << header.raw_width << "x"
           << header.raw_height << " raw_bytes=" << header.raw_bytes
           << " frame_type="
           << tof_net::cstr_to_string(header.frame_type, tof_net::kNameLen);
        setStatus(ss.str());
    }

    std::shared_ptr<aditof::Frame> frame;
    try {
        frame = computeTofiFrame(header, rawBytes);
    } catch (const std::exception &e) {
        frame = buildFallbackFrame(header, rawBytes, e.what());
    }

    if (frame) {
        m_queue.enqueue_latest(frame);
    }
}

std::shared_ptr<aditof::Frame> ADINetworkToFStream::computeTofiFrame(
    const tof_net::DataFramePayloadHeader &header,
    const std::vector<uint8_t> &rawBytes) {
    if ((rawBytes.size() % sizeof(uint16_t)) != 0) {
        throw std::runtime_error("Decoded RAW frame is not 16-bit aligned");
    }

    Config config;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        config = m_config;
    }

    const std::string frameType = makeFrameType(header, config);
    const uint16_t mode = makeMode(header, config);

    // Resolve INI against the DataFrame's actual frame_type, not the
    // GUI-selected mode (they can differ when the camera is configured
    // with a different mode than what the GUI picker shows).
    std::string resolvedIni = config.iniFile;
    if (resolvedIni.empty() ||
        resolvedIni.find(frameType) == std::string::npos) {
        resolvedIni = resolveIniFile(std::string(), frameType);
    }

    if (!ensureTofi(mode, frameType, resolvedIni, config.ccbFile,
                    config.cfgFile)) {
        throw std::runtime_error("TOFI context is not initialized");
    }

    std::vector<uint16_t> rawWords(rawBytes.size() / sizeof(uint16_t));
    std::memcpy(rawWords.data(), rawBytes.data(), rawBytes.size());
    std::cerr << "[viewer] TofiCompute input: " << rawWords.size()
              << " uint16 words, ctx=" << m_tofiContext << std::endl;

    std::lock_guard<std::mutex> lock(m_tofiMutex);
    TofiComputeContext *tofiContext =
        static_cast<TofiComputeContext *>(m_tofiContext);
    std::cerr << "[viewer] tofiContext->n_rows=" << tofiContext->n_rows
              << " n_cols=" << tofiContext->n_cols
              << " p_depth=" << (void*)tofiContext->p_depth_frame
              << " p_ab=" << (void*)tofiContext->p_ab_frame
              << " p_xyz=" << (void*)tofiContext->p_xyz_frame << std::endl;

    maskFloatingPointTrapsForTofi();
    std::cerr << "[viewer] calling TofiCompute..." << std::endl;
    const int computeStatus = TofiCompute(rawWords.data(), tofiContext, nullptr);
    std::cerr << "[viewer] TofiCompute returned " << computeStatus << std::endl;
    maskFloatingPointTrapsForTofi();
    if (computeStatus != 0) {
        std::ostringstream ss;
        ss << "TofiCompute failed with status " << computeStatus;
        throw std::runtime_error(ss.str());
    }

    const uint32_t width = tofiContext->n_cols;
    const uint32_t height = tofiContext->n_rows;
    if (width == 0 || height == 0) {
        throw std::runtime_error("TOFI returned an empty frame geometry");
    }

    if (header.frame_id == 1) {
        std::cout << "[viewer] TofiCompute succeeded: output="
                  << width << "x" << height
                  << " (raw was " << header.raw_width << "x" << header.raw_height
                  << ", " << header.raw_bytes << " bytes input)\n";
    }

    aditof::FrameDetails frameDetails;
    frameDetails.type = frameType;
    frameDetails.cameraMode = frameType;
    frameDetails.width = width;
    frameDetails.height = height;
    frameDetails.totalCaptures = 1;
    frameDetails.passiveIRCaptured = false;
    frameDetails.dataDetails.clear();

    addPlane(frameDetails.dataDetails, "depth", width, height,
             sizeof(uint16_t), 1);
    addPlane(frameDetails.dataDetails, "ir", width, height, sizeof(uint16_t),
             1);
    if (config.enableXyz && tofiContext->p_xyz_frame != nullptr) {
        addPlane(frameDetails.dataDetails, "xyz", width, height,
                 sizeof(uint16_t), 3);
    }

    auto frame = std::make_shared<aditof::Frame>();
    frame->setDetails(frameDetails);
    std::cerr << "[viewer] Frame created, w=" << width << " h=" << height << std::endl;

    const size_t pixelCount = static_cast<size_t>(width) * height;

    uint16_t *depthData = nullptr;
    aditof::Status ds = frame->getData("depth", &depthData);
    std::cerr << "[viewer] getData(depth) status=" << static_cast<int>(ds)
              << " ptr=" << (void*)depthData
              << " tofi_p_depth=" << (void*)tofiContext->p_depth_frame << std::endl;
    if (ds == aditof::Status::OK &&
        depthData != nullptr && tofiContext->p_depth_frame != nullptr) {
        std::memcpy(depthData, tofiContext->p_depth_frame,
                    pixelCount * sizeof(uint16_t));
    }

    uint16_t *irData = nullptr;
    if (frame->getData("ir", &irData) == aditof::Status::OK &&
        irData != nullptr && tofiContext->p_ab_frame != nullptr) {
        std::memcpy(irData, tofiContext->p_ab_frame,
                    pixelCount * sizeof(uint16_t));
    }

    if (header.frame_id == 1 && pixelCount > 0) {
        auto range16 = [pixelCount](const uint16_t *p) {
            uint16_t mn = 0xFFFF;
            uint16_t mx = 0;
            if (p == nullptr) {
                return std::pair<uint16_t, uint16_t>{0, 0};
            }
            for (size_t i = 0; i < pixelCount; ++i) {
                mn = std::min<uint16_t>(mn, p[i]);
                mx = std::max<uint16_t>(mx, p[i]);
            }
            return std::make_pair(mn, mx);
        };
        const auto depthRange = range16(tofiContext->p_depth_frame);
        const auto abRange = range16(tofiContext->p_ab_frame);
        std::ostringstream first;
        first << "First TOFI output: " << width << "x" << height
              << ", depth_range=[" << depthRange.first << "," << depthRange.second
              << "], ir_range=[" << abRange.first << "," << abRange.second << "]";
        std::cout << "[tof-net-viewer] " << first.str() << std::endl;
        LOG(INFO) << first.str();
    }

    uint16_t *xyzData = nullptr;
    if (config.enableXyz &&
        frame->getData("xyz", &xyzData) == aditof::Status::OK &&
        xyzData != nullptr && tofiContext->p_xyz_frame != nullptr) {
        std::memcpy(xyzData, tofiContext->p_xyz_frame,
                    static_cast<size_t>(width) * height * 3 * sizeof(int16_t));
    }

    std::ostringstream ss;
    ss << "Received RAW frame " << header.frame_id << ", TOFI " << width
       << "x" << height << ", mode=" << mode << ", frame_type="
       << frameType;
    setStatus(ss.str());

    return frame;
}

std::shared_ptr<aditof::Frame> ADINetworkToFStream::buildFallbackFrame(
    const tof_net::DataFramePayloadHeader &header,
    const std::vector<uint8_t> &rawBytes, const std::string &reason) {
    if ((rawBytes.size() % sizeof(uint16_t)) != 0 || header.raw_width == 0 ||
        header.raw_height == 0) {
        setStatus("Dropped RAW frame: " + reason);
        return nullptr;
    }

    Config config;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        config = m_config;
    }

    const std::string frameType = makeFrameType(header, config);
    const uint64_t expectedBytes = static_cast<uint64_t>(header.raw_width) *
                                   header.raw_height * sizeof(uint16_t);
    if (rawBytes.size() < expectedBytes) {
        setStatus("Dropped RAW frame: " + reason);
        return nullptr;
    }

    aditof::FrameDetails frameDetails;
    frameDetails.type = frameType;
    frameDetails.cameraMode = frameType;
    frameDetails.width = header.raw_width;
    frameDetails.height = header.raw_height;
    frameDetails.totalCaptures = 1;
    frameDetails.passiveIRCaptured = false;
    frameDetails.dataDetails.clear();
    addPlane(frameDetails.dataDetails, "depth", header.raw_width,
             header.raw_height, sizeof(uint16_t), 1);
    addPlane(frameDetails.dataDetails, "ir", header.raw_width, header.raw_height,
             sizeof(uint16_t), 1);

    auto frame = std::make_shared<aditof::Frame>();
    frame->setDetails(frameDetails);

    uint16_t *depthData = nullptr;
    uint16_t *irData = nullptr;
    if (frame->getData("depth", &depthData) == aditof::Status::OK &&
        depthData != nullptr) {
        std::memcpy(depthData, rawBytes.data(), static_cast<size_t>(expectedBytes));
    }
    if (frame->getData("ir", &irData) == aditof::Status::OK && irData != nullptr) {
        std::memcpy(irData, rawBytes.data(), static_cast<size_t>(expectedBytes));
    }

    setStatus("TOFI failed; displaying RAW fallback: " + reason);
    LOG(ERROR) << "TOFI failed; displaying RAW fallback: " << reason;
    return frame;
}

bool ADINetworkToFStream::ensureTofi(uint16_t mode,
                                     const std::string &frameType,
                                     const std::string &iniFile,
                                     const std::string &ccbFile,
                                     const std::string &cfgFile) {
    // ADSD3500 devices use the ISP-specific TOFI path (InitTofiConfig_isp)
    // which does not require a CFG file and is the same code path the SDK
    // uses internally for this hardware.
    bool adsd3500;
    {
        std::lock_guard<std::mutex> cfgLock(m_configMutex);
        adsd3500 = m_config.isAdsd3500;
    }
    if (adsd3500) {
        return ensureTofiIsp(mode, frameType, iniFile, ccbFile);
    }

    std::lock_guard<std::mutex> lock(m_tofiMutex);

    const std::string resolvedIniFile = resolveIniFile(iniFile, frameType);

    if (m_tofiContext != nullptr && m_tofiConfig != nullptr &&
        m_tofiMode == mode && m_tofiFrameType == frameType &&
        m_tofiIniFile == resolvedIniFile && m_tofiCcbFile == ccbFile &&
        m_tofiCfgFile == cfgFile) {
        return true;
    }

    releaseTofi();

    if (resolvedIniFile.empty()) {
        throw std::runtime_error(
            "No TOFI INI file configured. Set DEPTH_INI in tof-viewer_config.json or copy RawToDepthAdsd3500_<mode>.ini into ./config");
    }

    m_iniData = readWholeFile(resolvedIniFile);
    if (m_iniData.empty()) {
        throw std::runtime_error("TOFI INI file is empty: " + resolvedIniFile);
    }

    ConfigFileData iniData;
    iniData.p_data = m_iniData.data();
    iniData.size = m_iniData.size();

    ConfigFileData *ccbPtr = nullptr;
    ConfigFileData ccbData;
    if (!ccbFile.empty()) {
        m_ccbData = readWholeFile(ccbFile);
        if (m_ccbData.empty()) {
            throw std::runtime_error("TOFI CCB file is empty: " + ccbFile);
        }
        ccbData.p_data = m_ccbData.data();
        ccbData.size = m_ccbData.size();
        ccbPtr = &ccbData;
    }

    ConfigFileData *cfgPtr = nullptr;
    ConfigFileData cfgData;
    if (!cfgFile.empty()) {
        try {
            m_cfgData = readWholeFile(cfgFile);
        } catch (const std::exception &e) {
            m_cfgData.clear();
        }
        if (m_cfgData.empty()) {
            // ADSD3500 devices do not store CFG in EEPROM, and
            // the fallback CFG file may not exist. This is not fatal;
            // InitTofiConfig accepts cfgPtr=nullptr.
            std::ostringstream warn;
            warn << "TOFI CFG file not available: " << cfgFile
                 << " — proceeding without CFG (cfgPtr=nullptr)";
            setStatus(warn.str());
            LOG(WARNING) << warn.str();
        } else {
            cfgData.p_data = m_cfgData.data();
            cfgData.size = m_cfgData.size();
            cfgPtr = &cfgData;
        }
    }

    std::ostringstream inputs;
    inputs << "TOFI init input: mode=" << mode << ", frame_type=" << frameType
           << ", ini=" << resolvedIniFile << " (" << m_iniData.size() << " bytes)";
    if (ccbPtr != nullptr) {
        inputs << ", ccb=" << ccbFile << " (" << m_ccbData.size() << " bytes)";
    } else {
        inputs << ", ccb=<none>";
    }
    if (cfgPtr != nullptr) {
        inputs << ", cfg=" << cfgFile << " (" << m_cfgData.size() << " bytes)";
    } else {
        inputs << ", cfg=<none>";
    }
    setStatus(inputs.str());

    maskFloatingPointTrapsForTofi();
    uint32_t status = 0;
    m_tofiConfig = InitTofiConfig(ccbPtr, cfgPtr, &iniData, mode, &status);
    maskFloatingPointTrapsForTofi();
    if (m_tofiConfig == nullptr || status != 0) {
        std::ostringstream ss;
        ss << "InitTofiConfig failed with status " << status;
        throw std::runtime_error(ss.str());
    }

    TofiConfig *tofiConfig = static_cast<TofiConfig *>(m_tofiConfig);
    if (tofiConfig == nullptr || tofiConfig->p_tofi_cal_config == nullptr) {
        throw std::runtime_error("InitTofiConfig returned an invalid TOFI calibration config");
    }

    status = 0;
    // InitTofiCompute expects the calibrated TOFI configuration payload, not the
    // outer TofiConfig wrapper. Passing the wrapper pointer can crash inside the
    // closed-source compute library when the first RAW frame is processed.
    m_tofiContext = InitTofiCompute(tofiConfig->p_tofi_cal_config, &status);
    maskFloatingPointTrapsForTofi();
    if (m_tofiContext == nullptr || status != 0) {
        std::ostringstream ss;
        ss << "InitTofiCompute failed with status " << status;
        throw std::runtime_error(ss.str());
    }

    m_tofiMode = mode;
    m_tofiFrameType = frameType;
    m_tofiIniFile = resolvedIniFile;
    m_tofiCcbFile = ccbFile;
    m_tofiCfgFile = cfgFile;

    std::ostringstream ss;
    ss << "TOFI initialized: mode=" << mode << ", frame_type=" << frameType
       << ", ini=" << resolvedIniFile;
    if (!ccbFile.empty()) {
        ss << ", ccb=" << ccbFile;
    }
    if (!cfgFile.empty()) {
        ss << ", cfg=" << cfgFile;
    }
    setStatus(ss.str());
    return true;
}

bool ADINetworkToFStream::ensureTofiIsp(uint16_t /*mode*/,
                                         const std::string &frameType,
                                         const std::string &iniFile,
                                         const std::string &ccbFile) {
    std::lock_guard<std::mutex> lock(m_tofiMutex);

    const std::string resolvedIniFile = resolveIniFile(iniFile, frameType);
    const uint8_t convertedMode = convertModeName(frameType);

    // Cache check: if the TOFI context is already initialized with the same
    // parameters, skip re-initialization.
    if (m_tofiContext != nullptr && m_tofiConfig != nullptr &&
        m_tofiMode == convertedMode && m_tofiFrameType == frameType &&
        m_tofiIniFile == resolvedIniFile && m_tofiCcbFile == ccbFile) {
        return true;
    }

    releaseTofi();

    if (resolvedIniFile.empty()) {
        throw std::runtime_error(
            "No TOFI INI file configured. Set DEPTH_INI in "
            "tof-viewer_config.json or copy "
            "RawToDepthAdsd3500_<mode>.ini into ./config");
    }

    m_iniData = readWholeFile(resolvedIniFile);
    if (m_iniData.empty()) {
        throw std::runtime_error("TOFI INI file is empty: " + resolvedIniFile);
    }

    // Load CCB for cache-key tracking only. The XYZ dealias data used by
    // InitTofiConfig_isp is received separately via DealiasData messages
    // (sent by Machine A before streaming). This mirrors the SDK's ADSD3500
    // path where intrinsics+dealias are read from hardware cmd 0x01/0x02.
    if (ccbFile.empty()) {
        throw std::runtime_error(
            "ADSD3500 ISP TOFI path requires a CCB file (received from "
            "Machine A). No CCB file configured.");
    }

    m_ccbData = readWholeFile(ccbFile);
    if (m_ccbData.empty()) {
        throw std::runtime_error("TOFI CCB file is empty: " + ccbFile);
    }

    // Verify that dealias data for this mode was received from Machine A.
    if (m_xyzDealiasData[convertedMode].n_rows == 0 ||
        m_xyzDealiasData[convertedMode].n_cols == 0) {
        std::ostringstream ss;
        ss << "No dealias data received from Machine A for mode "
           << static_cast<int>(convertedMode) << " (" << frameType
           << "). Is tof_net_collect running an up-to-date build?";
        throw std::runtime_error(ss.str());
    }

    // Prepare depth INI data for InitTofiConfig_isp.
    // InitTofiConfig_isp stores a pointer into this buffer internally
    // (TofiConfig.p_tofi_config_str). We must keep it alive for the
    // lifetime of the TofiConfig/TofiComputeContext.
    m_ispIniCopy = m_iniData;  // copy, kept alive by member
    ConfigFileData depthIni = {m_ispIniCopy.data(), m_ispIniCopy.size()};

    std::ostringstream inputs;
    inputs << "TOFI ISP init: mode_name=" << frameType
           << ", converted_mode=" << static_cast<int>(convertedMode)
           << ", ini=" << resolvedIniFile << " (" << m_iniData.size()
           << " bytes), ccb=" << ccbFile << " (" << m_ccbData.size()
           << " bytes)"
           << "\n[viewer]   dealias[" << static_cast<int>(convertedMode)
           << "]: rows=" << m_xyzDealiasData[convertedMode].n_rows
           << " cols=" << m_xyzDealiasData[convertedMode].n_cols
           << " freqs=" << static_cast<int>(m_xyzDealiasData[convertedMode].n_freqs)
           << " fx=" << m_xyzDealiasData[convertedMode].camera_intrinsics.fx
           << " fy=" << m_xyzDealiasData[convertedMode].camera_intrinsics.fy
           << " depth_ini_bytes=" << m_iniData.size();
    setStatus(inputs.str());
    LOG(INFO) << inputs.str();
    std::cout << "[viewer] " << inputs.str() << std::endl;

    maskFloatingPointTrapsForTofi();
    uint32_t status = 0;
    // InitTofiConfig_isp: ADSD3500 ISP-specific TOFI configuration.
    // This is the same function the SDK uses internally for ADSD3500.
    m_tofiConfig = InitTofiConfig_isp(
        &depthIni, convertedMode, &status, m_xyzDealiasData);
    maskFloatingPointTrapsForTofi();

    if (m_tofiConfig == nullptr || status != 0) {
        std::ostringstream ss;
        ss << "InitTofiConfig_isp failed with status " << status
           << " (mode=" << static_cast<int>(convertedMode)
           << ", frame_type=" << frameType << ")";
        throw std::runtime_error(ss.str());
    }

    TofiConfig *tofiConfig = static_cast<TofiConfig *>(m_tofiConfig);
    if (tofiConfig == nullptr ||
        tofiConfig->p_tofi_cal_config == nullptr) {
        throw std::runtime_error(
            "InitTofiConfig_isp returned an invalid TOFI calibration config");
    }

    status = 0;
    m_tofiContext =
        InitTofiCompute(tofiConfig->p_tofi_cal_config, &status);
    maskFloatingPointTrapsForTofi();
    if (m_tofiContext == nullptr || status != 0) {
        std::ostringstream ss;
        ss << "InitTofiCompute failed with status " << status;
        throw std::runtime_error(ss.str());
    }

    // The opensource InitTofiCompute does NOT allocate output buffers or
    // set n_rows/n_cols.  We must do it ourselves, mirroring the SDK path
    // where the camera maps frame data pointers into the context.
    {
        TofiComputeContext *tc =
            static_cast<TofiComputeContext *>(m_tofiContext);
        uint32_t nRows = static_cast<uint32_t>(
            m_xyzDealiasData[convertedMode].n_rows);
        uint32_t nCols = static_cast<uint32_t>(
            m_xyzDealiasData[convertedMode].n_cols);
        tc->n_rows = nRows;
        tc->n_cols = nCols;

        if (nRows > 0 && nCols > 0) {
            size_t planePixels = static_cast<size_t>(nRows) * nCols;
            tc->p_depth_frame = new uint16_t[planePixels]();
            tc->p_ab_frame = new uint16_t[planePixels]();
            tc->p_conf_frame = new float[planePixels]();
            tc->p_xyz_frame = new int16_t[planePixels * 3]();
        }

        std::cout << "[viewer] Output buffers allocated: "
                  << nCols << "x" << nRows
                  << " depth=" << (void *)tc->p_depth_frame
                  << " ab=" << (void *)tc->p_ab_frame
                  << " xyz=" << (void *)tc->p_xyz_frame
                  << std::endl;
    }

    m_tofiMode = convertedMode;
    m_tofiFrameType = frameType;
    m_tofiIniFile = resolvedIniFile;
    m_tofiCcbFile = ccbFile;
    m_tofiCfgFile.clear(); // ADSD3500 ISP path does not use CFG

    std::ostringstream ss;
    ss << "TOFI ISP initialized: mode=" << static_cast<int>(convertedMode)
       << " (" << frameType << "), ini=" << resolvedIniFile
       << ", ccb=" << ccbFile;
    setStatus(ss.str());
    LOG(INFO) << ss.str();
    return true;
}

void ADINetworkToFStream::releaseTofi() {
    if (m_tofiContext != nullptr) {
        TofiComputeContext *tc =
            static_cast<TofiComputeContext *>(m_tofiContext);
        delete[] tc->p_depth_frame;
        delete[] tc->p_ab_frame;
        delete[] reinterpret_cast<float *>(tc->p_conf_frame);
        delete[] tc->p_xyz_frame;
        FreeTofiCompute(tc);
        m_tofiContext = nullptr;
    }
    if (m_tofiConfig != nullptr) {
        FreeTofiConfig(static_cast<TofiConfig *>(m_tofiConfig));
        m_tofiConfig = nullptr;
    }
    m_tofiMode = 0;
    m_tofiFrameType.clear();
    m_tofiIniFile.clear();
    m_tofiCcbFile.clear();
    m_tofiCfgFile.clear();
    m_iniData.clear();
    m_ccbData.clear();
    m_cfgData.clear();
    m_ispIniCopy.clear();
}

std::shared_ptr<aditof::Frame> ADINetworkToFStream::buildEmptyFrame() {
    aditof::FrameDetails frameDetails;
    frameDetails.type = "network-waiting";
    frameDetails.cameraMode = "network-waiting";
    frameDetails.width = 1;
    frameDetails.height = 1;
    frameDetails.totalCaptures = 1;
    frameDetails.passiveIRCaptured = false;
    frameDetails.dataDetails.clear();
    addPlane(frameDetails.dataDetails, "depth", 1, 1, sizeof(uint16_t), 1);
    addPlane(frameDetails.dataDetails, "ir", 1, 1, sizeof(uint16_t), 1);

    auto frame = std::make_shared<aditof::Frame>();
    frame->setDetails(frameDetails);

    uint16_t *depthData = nullptr;
    if (frame->getData("depth", &depthData) == aditof::Status::OK &&
        depthData != nullptr) {
        depthData[0] = 0;
    }

    uint16_t *irData = nullptr;
    if (frame->getData("ir", &irData) == aditof::Status::OK &&
        irData != nullptr) {
        irData[0] = 0;
    }

    return frame;
}

std::vector<uint8_t> ADINetworkToFStream::readWholeFile(
    const std::string &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>());
}

void ADINetworkToFStream::setStatus(const std::string &text) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        changed = (m_statusText != text);
        m_statusText = text;
    }

    // Keep high-rate per-frame updates in the GUI only, but print connection,
    // listening and error transitions to the terminal/log so headless SSH runs
    // can verify whether Machine B is actually accepting TCP connections.
    const bool highRateFrameUpdate =
        text.compare(0, 18, "Received RAW frame") == 0 ||
        text.compare(0, 17, "Dropped RAW frame") == 0;
    if (changed && !highRateFrameUpdate) {
        std::cout << "[tof-net-viewer] " << text << std::endl;
        LOG(INFO) << text;
    }
}
