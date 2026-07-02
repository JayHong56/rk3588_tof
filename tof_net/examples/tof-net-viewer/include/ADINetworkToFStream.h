/********************************************************************************/
/*                                                                              */
/* Copyright (c) 2020 Analog Devices, Inc. All Rights Reserved.                 */
/* Modified for network-only RAW ToF viewing.                                   */
/*                                                                              */
/********************************************************************************/
#ifndef ADINETWORKTOFSTREAM_H
#define ADINETWORKTOFSTREAM_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <aditof/frame.h>

#include "safequeue.h"
#include "tof_net/protocol.hpp"
#include "tof_net/socket.hpp"

// For TofiXYZDealiasData used by InitTofiConfig_isp (ADSD3500 ISP path)
#if __has_include(<tofi/tofi_camera_intrinsics.h>)
#include <tofi/tofi_camera_intrinsics.h>
#elif __has_include(<tofi_camera_intrinsics.h>)
#include <tofi_camera_intrinsics.h>
#endif


namespace adicontroller {

class ADINetworkToFStream {
  public:
    struct Config {
        std::string listenIp = "0.0.0.0";
        uint16_t listenPort = 5000;
        // Fallback CCB/CFG paths. They are used only until Machine A sends the live
        // module files over the network.
        std::string ccbFile;
        std::string cfgFile;
        // Where Machine B saves the CCB/CFG received from Machine A.
        std::string receivedCcbFile = "./config/received_machine_a_module.ccb";
        std::string receivedCfgFile = "./config/received_machine_a_module.cfg";
        std::string iniFile;
        std::string frameType = "lr-mixed";
        uint16_t mode = 0;
        uint32_t requestedFps = 0;
        bool enableXyz = true;
        // When true, use InitTofiConfig_isp (ADSD3500 ISP path) instead of
        // the generic InitTofiConfig. The ISP path requires XYZ dealias data
        // extracted from the CCB and does not use a separate CFG file.
        bool isAdsd3500 = false;
    };

    ADINetworkToFStream();
    ~ADINetworkToFStream();

    void configure(const Config &config);
    // Start/stop the TCP listener. In this topology Machine A's
    // tof_net_collect is started first and repeatedly connects to this
    // listener; no capture command is sent until startRemoteCapture().
    void start();
    void stop();

    // Called by the Play/Stop buttons. These send commands to Machine A
    // without tearing down the already-established TCP connection.
    bool startRemoteCapture();
    bool stopRemoteCapture();

    void requestFrame();
    void setFrameType(const std::string &frameType);

    std::shared_ptr<aditof::Frame> getFrame();

    bool isRunning() const;
    bool hasClient() const;
    std::string statusText() const;

  private:
    void workerLoop();
    bool sendStartCaptureCommand();
    bool sendStopCaptureCommand();
    void handleDataFrame(const tof_net::Message &message);
    void handleCcbFile(const tof_net::Message &message);
    void handleCfgFile(const tof_net::Message &message);
    void handleDealiasData(const tof_net::Message &message);
    std::shared_ptr<aditof::Frame>
    computeTofiFrame(const tof_net::DataFramePayloadHeader &header,
                     const std::vector<uint8_t> &rawBytes);
    std::shared_ptr<aditof::Frame>
    buildFallbackFrame(const tof_net::DataFramePayloadHeader &header,
                       const std::vector<uint8_t> &rawBytes,
                       const std::string &reason);

    bool ensureTofi(uint16_t mode, const std::string &frameType,
                    const std::string &iniFile, const std::string &ccbFile,
                    const std::string &cfgFile);
    // ADSD3500 ISP path: InitTofiConfig_isp with XYZ dealias data from CCB.
    bool ensureTofiIsp(uint16_t mode, const std::string &frameType,
                       const std::string &iniFile, const std::string &ccbFile);
    // Convert a mode name (e.g. "lr-qnative") to the TOFI mode ID used by
    // InitTofiConfig_isp. Mirrors the SDK's ModeInfo::convertCameraMode.
    static uint8_t convertModeName(const std::string &frameType);
    void releaseTofi();
    static std::shared_ptr<aditof::Frame> buildEmptyFrame();
    static std::vector<uint8_t> readWholeFile(const std::string &path);
    void setStatus(const std::string &text);

  private:
    mutable std::mutex m_configMutex;
    Config m_config;

    mutable std::mutex m_statusMutex;
    std::string m_statusText;

    std::atomic<bool> m_stopFlag;
    std::atomic<bool> m_running;
    std::thread m_workerThread;

    tof_net::Socket m_listenSocket;
    tof_net::Socket m_clientSocket;
    mutable std::mutex m_socketMutex;
    std::atomic<bool> m_clientConnected;
    std::atomic<bool> m_remoteCapturing;
    std::atomic<bool> m_waitingForFirstFrame;
    std::atomic<bool> m_autoStartSent;
    std::chrono::steady_clock::time_point m_lastStartCommandTime;
    std::chrono::steady_clock::time_point m_lastTofiFrameTime;
    SafeQueue<std::shared_ptr<aditof::Frame>> m_queue;
    std::shared_ptr<aditof::Frame> m_lastFrame;

    std::mutex m_tofiMutex;
    void *m_tofiConfig;
    void *m_tofiContext;
    uint16_t m_tofiMode;
    std::string m_tofiFrameType;
    std::string m_tofiIniFile;
    std::string m_tofiCcbFile;
    std::string m_tofiCfgFile;
    std::string m_receivedCcbFile;
    std::string m_receivedCfgFile;
    std::vector<uint8_t> m_iniData;
    std::vector<uint8_t> m_ccbData;
    std::vector<uint8_t> m_cfgData;
    // Separate INI buffer for the ISP path. InitTofiConfig_isp stores a
    // pointer into this data (TofiConfig.p_tofi_config_str); it must
    // outlive the TofiConfig/TofiComputeContext.
    std::vector<uint8_t> m_ispIniCopy;
    // XYZ dealias data extracted from CCB, used by InitTofiConfig_isp for
    // ADSD3500. Array of 11 entries — one per supported mode.
    // Mirrors SDK's TofiXYZDealiasData tempDealiasStruct[11].
    TofiXYZDealiasData m_xyzDealiasData[11];
};

} // namespace adicontroller

#endif // ADINETWORKTOFSTREAM_H
