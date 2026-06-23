#include "tof_net/protocol.hpp"
#include "tof_net/socket.hpp"
#include "tof_net/zstd_codec.hpp"

#include <aditof/camera.h>
#include <aditof/depth_sensor_interface.h>
#include <aditof/frame.h>
#include <aditof/frame_definitions.h>
#include <aditof/status_definitions.h>
#include <aditof/system.h>
#include <aditof/version.h>

// For TofiXYZDealiasData / CameraIntrinsics used by adsd3500 dealias export
#if __has_include(<tofi/tofi_camera_intrinsics.h>)
#include <tofi/tofi_camera_intrinsics.h>
#elif __has_include(<tofi_camera_intrinsics.h>)
#include <tofi_camera_intrinsics.h>
#endif

#include "mode_info.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace tof_net;

namespace {

constexpr uint32_t EMBED_HDR_LENGTH = 128;
std::atomic<bool> g_run{true};

struct Args {
    // Same meaning as examples/data_collect FILE:
    // ADI camera initialization JSON, for example config/config_crosby_adsd3500_new_modes.json.
    std::string initialization_config;

    // data_collect-compatible camera options. Keep these option names unchanged.
    std::string camera_ip;        // data_collect: --ip <ip>
    uint16_t mode = 0;            // data_collect: --m <mode>
    std::string frame_type = "raw"; // data_collect: --ft <frame_type>; accepted, forced to raw here.
    // data_collect-compatible sensor configuration. For this raw-only relay,
    // standard-raw is the safest default; standard can route a different
    // processed/interleaved layout on some SDK/firmware combinations.
    std::string sensor_configuration = "standard-raw";
    std::string firmware;         // data_collect: --fw <firmware>
    uint32_t fps = 0;             // data_collect: --fps <setfps>; accepted, not applied, same as #if 0 in data_collect.
    std::string ccb_out;          // data_collect: --ccb <FILE>
    uint32_t warmup_time = 0;     // data_collect: --wt <warmup>
    uint32_t ext_fsync = 0;       // data_collect: --ext_fsync <0|1>
    uint32_t n_frames = 0;        // data_collect: --n <ncapture>; 0 means stream until StopCapture.

    // tof_net_collect-specific network/compression options.
    std::string server_ip = "127.0.0.1";
    std::string bind_ip;
    uint16_t port = 5000;
    int zstd_level = 1;
    bool reconnect = true;
};

void on_signal(int) { g_run = false; }

bool file_exists(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return static_cast<bool>(f);
}

std::vector<uint8_t> read_whole_file(const std::string &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>());
}

bool is_absolute_path(const std::string &path) {
    if (path.empty()) return false;
#ifdef _WIN32
    return path.size() > 2 && path[1] == ':';
#else
    return path[0] == '/';
#endif
}

std::string dirname_of(const std::string &path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

std::string join_path(const std::string &base, const std::string &rel) {
    if (base.empty() || base == ".") return rel;
    if (base.back() == '/' || base.back() == '\\') return base + rel;
    return base + "/" + rel;
}

std::string resolve_config_path(const std::string &requested, const char *argv0) {
    if (requested.empty()) return requested;
    if (file_exists(requested)) return requested;
    if (!is_absolute_path(requested)) {
        const std::string alt = join_path(dirname_of(argv0 ? argv0 : ""), requested);
        if (file_exists(alt)) return alt;
    }
    return requested;
}

void usage() {
    std::cout <<
        "tof_net_collect FILE [data_collect-options] [network-options]\n"
        "\n"
        "This program is built from the examples/data_collect acquisition model.\n"
        "It always captures raw frames with enableDepthCompute=off, compresses them\n"
        "losslessly, and sends them to tof_net_viewer. It does not compute depth.\n"
        "\n"
        "Arguments:\n"
        "  FILE                         ADI camera initialization JSON, same as data_collect.\n"
        "                               Example: config/config_crosby_adsd3500_new_modes.json\n"
        "\n"
        "data_collect-compatible options:\n"
        "  --ip <ip>                     Camera IP, same spelling as data_collect.\n"
        "  --m <mode>                    Camera mode number. Default: 0.\n"
        "  --ft <frame_type>             Accepted for compatibility. Forced to raw in this app.\n"
        "  --fw <firmware>               Firmware path, same as data_collect.\n"
        "  --ic <configuration>          Sensor configuration. Default: standard-raw.\n"
        "  --fps <setfps>                Accepted for compatibility.\n"
        "  --ccb <FILE>                  Save module CCB to FILE, same as data_collect.\n"
        "  --ext_fsync <0|1>             Sync mode, same as data_collect.\n"
        "  --wt <warmup>                 Warmup seconds, same as data_collect.\n"
        "  --n <ncapture>                Optional max frame count. 0 means stream until StopCapture.\n"
        "\n"
        "tof_net_collect network/compression options:\n"
        "  --server-ip <B_IP>            Machine B / tof_net_viewer IP.\n"
        "  --bind-ip <A_IP>              Bind outbound TCP socket to Machine A IP.\n"
        "  --port <PORT>                 TCP port. Default: 5000.\n"
        "  --zstd-level <N>              Zstd compression level. Default: 1.\n"
        "  --no-reconnect                Exit when the B connection drops.\n"
        "  -h, --help                    Show this help.\n";
}

Args parse_args(int argc, char **argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto need = [&](const char *name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (k == "-h" || k == "--help") { usage(); std::exit(0); }
        // data_collect-compatible options
        else if (k == "--ip") a.camera_ip = need("--ip");
        else if (k == "--m") a.mode = static_cast<uint16_t>(std::stoi(need("--m")));
        else if (k == "--ft") a.frame_type = need("--ft");
        else if (k == "--fw") a.firmware = need("--fw");
        else if (k == "--ic") a.sensor_configuration = need("--ic");
        else if (k == "--fps") a.fps = static_cast<uint32_t>(std::stoul(need("--fps")));
        else if (k == "--ccb") a.ccb_out = need("--ccb");
        else if (k == "--wt") a.warmup_time = static_cast<uint32_t>(std::stoul(need("--wt")));
        else if (k == "--ext_fsync") a.ext_fsync = static_cast<uint32_t>(std::stoul(need("--ext_fsync")));
        else if (k == "--n") a.n_frames = static_cast<uint32_t>(std::stoul(need("--n")));
        // tof_net_collect-specific options
        else if (k == "--server-ip") a.server_ip = need("--server-ip");
        else if (k == "--bind-ip") a.bind_ip = need("--bind-ip");
        else if (k == "--port") a.port = static_cast<uint16_t>(std::stoi(need("--port")));
        else if (k == "--zstd-level") a.zstd_level = std::stoi(need("--zstd-level"));
        else if (k == "--no-reconnect") a.reconnect = false;
        else if (!k.empty() && k[0] != '-') {
            if (!a.initialization_config.empty()) throw std::runtime_error("multiple FILE arguments provided");
            a.initialization_config = k;
        } else {
            throw std::runtime_error("unknown option: " + k);
        }
    }
    if (a.initialization_config.empty()) {
        throw std::runtime_error("missing FILE argument: ADI camera initialization JSON is required");
    }
    a.initialization_config = resolve_config_path(a.initialization_config, argc > 0 ? argv[0] : nullptr);
    if (a.frame_type != "raw") {
        std::cerr << "Warning: tof_net_collect does not compute depth; --ft " << a.frame_type
                  << " is accepted for data_collect compatibility but forced to raw.\n";
        a.frame_type = "raw";
    }
    return a;
}

class CameraRawSource {
  public:
    explicit CameraRawSource(const Args &args) : args_(args) {}

    void initialize() {
        aditof::System system;
        std::vector<std::shared_ptr<aditof::Camera>> cameras;

        if (args_.camera_ip.empty()) {
            system.getCameraList(cameras);
        } else {
            // Same as examples/data_collect on rel-4.2.1.
            system.getCameraListAtIp(cameras, args_.camera_ip);
        }

        if (cameras.empty()) {
            throw std::runtime_error("No ADI ToF camera found");
        }
        camera_ = cameras.front();

        // SDK 4.2 in this tree exposes Camera::initialize() with no arguments.
        // Keep the initialization JSON path compatible by setting the standard
        // initialization_config control before initialize().
        auto st = camera_->setControl("initialization_config", args_.initialization_config);
        if (st != aditof::Status::OK) throw std::runtime_error("setControl(initialization_config) failed");

        st = camera_->initialize();
        if (st != aditof::Status::OK) throw std::runtime_error("camera initialize failed");

        // Keep --ic accepted for command-line compatibility, but do not call
        // Camera::setSensorConfiguration(): this SDK version does not expose
        // that method. Try the equivalent control name only as a best-effort
        // runtime hint; failures are non-fatal.
        if (!args_.sensor_configuration.empty()) {
            auto icSt = camera_->setControl("sensorConfiguration", args_.sensor_configuration);
            if (icSt != aditof::Status::OK) {
                std::cerr << "Warning: sensorConfiguration=" << args_.sensor_configuration
                          << " was not accepted by this SDK/backend; continuing\n";
            } else {
                std::cout << "Sensor configuration control: " << args_.sensor_configuration << "\n";
            }
        }

        aditof::CameraDetails cameraDetails;
        camera_->getDetails(cameraDetails);
        std::cout << "Camera initialized. SDK " << aditof::getApiVersion() << "\n";
        std::cout << "SD card image version: " << cameraDetails.sdCardImageVersion << "\n";
        std::cout << "Kernel version: " << cameraDetails.kernelVersion << "\n";
        std::cout << "U-Boot version: " << cameraDetails.uBootVersion << "\n";

        if (!args_.firmware.empty()) {
            st = camera_->setControl("updateAdsd3500Firmware", args_.firmware);
            if (st != aditof::Status::OK) throw std::runtime_error("updateAdsd3500Firmware failed");
            throw std::runtime_error("Firmware updated; reboot the board before capturing");
        }

        std::vector<std::string> frameTypes;
        st = camera_->getAvailableFrameTypes(frameTypes);
        if (st != aditof::Status::OK || frameTypes.empty()) {
            throw std::runtime_error("camera getAvailableFrameTypes failed");
        }

        std::cout << "[collect] Available frame types (" << frameTypes.size() << "):\n";
        for (const auto &ft : frameTypes) {
            auto info = ModeInfo::getInstance()->getModeInfo(ft);
            std::cout << "[collect]   " << ft
                      << " modeId=" << static_cast<int>(info.mode)
                      << " w=" << info.width << " h=" << info.height
                      << " subframes=" << static_cast<int>(info.subframes)
                      << " passive_ir=" << static_cast<int>(info.passive_ir)
                      << "\n";
        }

        st = camera_->getFrameTypeNameFromId(args_.mode, mode_name_);
        if (st != aditof::Status::OK || mode_name_.empty()) {
            throw std::runtime_error("invalid --m mode for this camera");
        }

        sensor_ = camera_->getSensor();
        if (!sensor_) throw std::runtime_error("camera getSensor failed");
        st = sensor_->getName(sensor_name_);
        if (st != aditof::Status::OK) sensor_name_.clear();

        std::cout << "[collect] Sensor name: " << sensor_name_ << "\n";
        std::cout << "[collect] Selected mode: --m " << args_.mode
                  << " -> " << mode_name_ << "\n";
        std::cout << "[collect] Sensor config: " << args_.sensor_configuration << "\n";
        std::cout << "[collect] Server IP: " << args_.server_ip << ":" << args_.port
                  << ", bind: " << (args_.bind_ip.empty() ? "auto" : args_.bind_ip) << "\n";
        std::cout << "[collect] Zstd level: " << args_.zstd_level
                  << ", reconnect: " << (args_.reconnect ? "yes" : "no") << "\n";
        std::cout << "[collect] Camera ready, waiting for Machine B viewer...\n";

        // This is the key split-app difference from tof-viewer:
        // Machine A must not compute depth. It captures raw and sends it.
        st = camera_->setControl("enableDepthCompute", "off");
        if (st != aditof::Status::OK) throw std::runtime_error("setControl(enableDepthCompute=off) failed");

        // SDK 4.2 exposes setMode(string) rather than setMode(id). Use the
        // resolved mode name through setFrameType(), matching the pre-v11
        // collector path that compiles against this SDK.
        st = camera_->setFrameType(mode_name_);
        if (st != aditof::Status::OK) throw std::runtime_error("camera setFrameType failed: " + mode_name_);

        if (!args_.ccb_out.empty()) {
            st = camera_->setControl("saveModuleCCB", args_.ccb_out);
            if (st != aditof::Status::OK) {
                std::cerr << "Warning: failed to save module CCB to " << args_.ccb_out << "\n";
            }
        }

        std::cout << "Mode: " << args_.mode << " -> " << mode_name_ << "\n";
        std::cout << "Frame type: raw, depth compute: off\n";
    }

    struct RawFrame {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bytes = 0;
        uint16_t mode = 0;
        std::string frame_type; // camera mode name: lr-qnative, sr-native, etc.
        uint32_t subelement_size = 0;
        uint32_t subelements_per_element = 0;
        std::vector<uint8_t> data;
    };

    struct ModuleCcb {
        std::string path;
        std::vector<uint8_t> data;
    };

    struct DealiasEntry {
        std::string frame_type;
        std::vector<uint8_t> data;  // raw TofiXYZDealiasData bytes
    };

    // Export per-mode intrinsics + dealias data directly from ADSD3500
    // hardware (cmd 0x01 + 0x02). This mirrors the SDK's initialization
    // sequence in CameraItof that populates m_xyz_dealias_data[mode].
    std::vector<DealiasEntry> export_dealias_data() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!camera_ || !sensor_) {
            throw std::runtime_error("camera not initialized");
        }
        if (sensor_name_ != "adsd3500") {
            std::cout << "Dealias export only supported for adsd3500; skipping\n";
            return {};
        }

        // Get all available frame types from the camera
        std::vector<std::string> frameTypes;
        auto st = camera_->getAvailableFrameTypes(frameTypes);
        if (st != aditof::Status::OK || frameTypes.empty()) {
            throw std::runtime_error("getAvailableFrameTypes failed");
        }

        std::vector<DealiasEntry> entries;
        for (const auto &ft : frameTypes) {
            uint8_t mode = ModeInfo::getInstance()
                               ->getModeInfo(ft)
                               .mode;

            uint8_t intrinsics[56] = {0};
            uint8_t dealiasParams[32] = {0};

            intrinsics[0] = mode;
            dealiasParams[0] = mode;

            st = sensor_->adsd3500_read_payload_cmd(0x01, intrinsics, 56);
            if (st != aditof::Status::OK) {
                std::cerr << "Warning: failed to read intrinsics (cmd 0x01) for "
                          << ft << "\n";
                continue;
            }
            st = sensor_->adsd3500_read_payload_cmd(0x02, dealiasParams, 32);
            if (st != aditof::Status::OK) {
                std::cerr << "Warning: failed to read dealias params (cmd 0x02) for "
                          << ft << "\n";
                continue;
            }

            TofiXYZDealiasData dealiasStruct;
            std::memset(&dealiasStruct, 0, sizeof(dealiasStruct));
            std::memcpy(&dealiasStruct, dealiasParams,
                        sizeof(TofiXYZDealiasData) - sizeof(CameraIntrinsics));
            std::memcpy(&dealiasStruct.camera_intrinsics, intrinsics,
                        sizeof(CameraIntrinsics));

            std::cout << "[collect] Dealias for " << ft
                      << " (mode=" << static_cast<int>(mode) << "):"
                      << " rows=" << dealiasStruct.n_rows
                      << " cols=" << dealiasStruct.n_cols
                      << " freqs=" << static_cast<int>(dealiasStruct.n_freqs)
                      << " row_bin=" << static_cast<int>(dealiasStruct.row_bin_factor)
                      << " col_bin=" << static_cast<int>(dealiasStruct.col_bin_factor)
                      << " sensor=" << dealiasStruct.n_sensor_rows
                      << "x" << dealiasStruct.n_sensor_cols
                      << " fx=" << dealiasStruct.camera_intrinsics.fx
                      << " fy=" << dealiasStruct.camera_intrinsics.fy
                      << " cx=" << dealiasStruct.camera_intrinsics.cx
                      << " cy=" << dealiasStruct.camera_intrinsics.cy
                      << "\n";

            DealiasEntry entry;
            entry.frame_type = ft;
            entry.data.assign(reinterpret_cast<const uint8_t *>(&dealiasStruct),
                              reinterpret_cast<const uint8_t *>(&dealiasStruct) +
                                  sizeof(dealiasStruct));
            entries.push_back(std::move(entry));

            std::cout << "Exported dealias data for " << ft
                      << " (mode=" << static_cast<int>(mode) << ", "
                      << entry.data.size() << " bytes)\n";
        }
        return entries;
    }

    ModuleCcb export_module_ccb() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!camera_) {
            throw std::runtime_error("camera not initialized");
        }

        std::vector<std::string> candidates;
        if (!args_.ccb_out.empty()) {
            candidates.push_back(args_.ccb_out);
        }
        candidates.push_back("/tmp/tof_net_collect_module_" +
                             std::to_string(static_cast<long long>(::getpid())) +
                             ".ccb");
        candidates.push_back("./tof_net_collect_module.ccb");

        std::string lastError;
        for (const std::string &path : candidates) {
            try {
                std::remove(path.c_str());
                auto st = camera_->setControl("saveModuleCCB", path);
                if (st != aditof::Status::OK) {
                    lastError = "saveModuleCCB failed for " + path;
                    std::cerr << "Warning: " << lastError << "\n";
                }

                if (!file_exists(path)) {
                    if (lastError.empty()) {
                        lastError = "saveModuleCCB did not create " + path;
                    }
                    continue;
                }

                ModuleCcb out;
                out.path = path;
                out.data = read_whole_file(path);
                if (out.data.empty()) {
                    lastError = "exported module CCB is empty: " + path;
                    continue;
                }

                std::cout << "Exported current module CCB: " << path
                          << " (" << out.data.size() << " bytes)\n";
                return out;
            } catch (const std::exception &e) {
                lastError = e.what();
            }
        }

        throw std::runtime_error("failed to export current module CCB: " + lastError);
    }

    // Switch the camera to a different frame type at runtime, preserving
    // all other initialization (enableDepthCompute=off, sensor config, etc.).
    bool switchMode(const std::string &frameType) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!camera_) return false;
        if (frameType == mode_name_) return true;

        stop_locked();
        auto st = camera_->setFrameType(frameType);
        if (st != aditof::Status::OK) {
            std::cerr << "[collect] setFrameType(" << frameType
                      << ") failed\n";
            return false;
        }
        mode_name_ = frameType;
        // Update args_.mode so DataFrame headers carry the correct mode ID.
        args_.mode = ModeInfo::getInstance()->getModeInfo(frameType).mode;
        std::cout << "[collect] Switched to mode: " << mode_name_
                  << " (modeId=" << args_.mode << ")\n";
        return true;
    }

    void start() {
        std::lock_guard<std::mutex> lock(mu_);
        stop_locked();
        if (!camera_) throw std::runtime_error("camera not initialized");

        auto st = camera_->start();
        if (st != aditof::Status::OK) throw std::runtime_error("camera start failed");

        if (args_.ext_fsync == 0) {
            camera_->setControl("syncMode", "0, 0"); // Master, timer driven.
        } else if (args_.ext_fsync == 1) {
            camera_->setControl("syncMode", "2, 0"); // Slave.
        }

        capturing_ = true;

        if (args_.warmup_time > 0) {
            std::cout << "Warmup for " << args_.warmup_time << " seconds...\n";
            const auto warmup_start = std::chrono::steady_clock::now();
            while (g_run) {
                (void)get_frame_locked();
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - warmup_start).count();
                if (elapsed >= static_cast<long>(args_.warmup_time)) break;
            }
        }

        // data_collect drops the first frame before saving. Keep the same
        // behavior here; the first frame after start can carry stale or partially
        // initialized raw payload on some firmware versions.
        try {
            (void)get_frame_locked();
            std::cout << "Dropped first RAW frame after camera start\n";
        } catch (const std::exception &e) {
            std::cerr << "Warning: failed to drop first frame: " << e.what() << "\n";
        }
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mu_);
        stop_locked();
    }

    RawFrame get_frame() {
        std::lock_guard<std::mutex> lock(mu_);
        return get_frame_locked();
    }

    uint32_t max_frames() const { return args_.n_frames; }

  private:
    static uint32_t checked_raw_byte_count(const aditof::FrameDataDetails &details) {
        const uint64_t bytes = static_cast<uint64_t>(details.width) *
                               static_cast<uint64_t>(details.height) *
                               static_cast<uint64_t>(details.subelementSize) *
                               static_cast<uint64_t>(details.subelementsPerElement);
        if (bytes == 0 || bytes > 0xFFFFFFFFull) {
            throw std::runtime_error("raw frame plane size is invalid or too large");
        }
        return static_cast<uint32_t>(bytes);
    }

    bool get_raw_plane_details(const aditof::FrameDetails &details,
                               aditof::FrameDataDetails &rawDetails) const {
        for (const auto &plane : details.dataDetails) {
            if (plane.type == "raw") {
                rawDetails = plane;
                return true;
            }
        }
        return false;
    }

    uint32_t determine_subframes(const aditof::Frame &frame,
                                 const aditof::FrameDetails &details) {
        uint32_t subFrames = 0;
        std::string imagerType;
        auto st = camera_->getControl("imagerType", imagerType);
        if (st != aditof::Status::OK) {
            throw std::runtime_error("failed to get imagerType");
        }

        if (sensor_name_ == "adsd3500") {
            if (imagerType == "1") {
                if (mode_name_ == "lr-native" || mode_name_ == "mp") {
                    subFrames = 8;
                } else if (mode_name_ == "sr-native") {
                    subFrames = 6;
                } else {
                    subFrames = 5;
                }
            } else {
                subFrames = 5;
            }
        } else {
            std::string attrVal;
            st = frame.getAttribute("total_captures", attrVal);
            if (st != aditof::Status::OK || attrVal.empty()) {
                throw std::runtime_error("failed to get total_captures frame attribute");
            }
            subFrames = static_cast<uint32_t>(std::stoul(attrVal));
            // Same as data_collect: attribute is in 16-bit captures; raw size is counted in bytes.
            subFrames *= 2;
        }

        if (mode_name_ == "pcm-native") {
            subFrames = 2;
        }

        if (subFrames == 0 || details.width == 0 || details.height == 0) {
            throw std::runtime_error("invalid raw frame geometry");
        }
        return subFrames;
    }

    RawFrame get_frame_locked() {
        if (!capturing_) throw std::runtime_error("not capturing");

        aditof::Frame frame;
        auto st = camera_->requestFrame(&frame);
        if (st != aditof::Status::OK) throw std::runtime_error("camera requestFrame failed");

        aditof::FrameDetails details;
        st = frame.getDetails(details);
        if (st != aditof::Status::OK) throw std::runtime_error("frame getDetails failed");

        aditof::FrameDataDetails rawDetails;
        rawDetails.width = 0;
        rawDetails.height = 0;
        rawDetails.subelementSize = 0;
        rawDetails.subelementsPerElement = 0;

        uint32_t rawBytes = 0;
        if (get_raw_plane_details(details, rawDetails)) {
            rawBytes = checked_raw_byte_count(rawDetails);
        } else {
            // Older SDKs may not expose plane metadata for raw. Fall back to
            // the data_collect-compatible ADSD3500 byte-per-pixel table.
            const uint32_t subFrames = determine_subframes(frame, details);
            const uint64_t raw_size_u64 = static_cast<uint64_t>(details.height) *
                                          static_cast<uint64_t>(details.width) *
                                          static_cast<uint64_t>(subFrames);
            if (raw_size_u64 == 0 || raw_size_u64 > 0xFFFFFFFFull) {
                throw std::runtime_error("raw frame size is invalid or too large");
            }
            rawBytes = static_cast<uint32_t>(raw_size_u64);
            rawDetails.width = details.width;
            rawDetails.height = details.height;
        }

        uint16_t *pData = nullptr;
        st = frame.getData("raw", &pData);
        if (st != aditof::Status::OK || pData == nullptr) {
            throw std::runtime_error("frame getData(raw) failed");
        }

        RawFrame out;
        out.width = rawDetails.width ? rawDetails.width : details.width;
        out.height = rawDetails.height ? rawDetails.height : details.height;
        out.bytes = rawBytes;
        out.subelement_size = rawDetails.subelementSize;
        out.subelements_per_element = rawDetails.subelementsPerElement;
        out.mode = args_.mode;
        out.frame_type = mode_name_;
        out.data.resize(out.bytes);
        std::memcpy(out.data.data(), reinterpret_cast<const uint8_t *>(pData), out.bytes);
        return out;
    }

    void stop_locked() {
        if (capturing_ && camera_) {
            camera_->stop();
        }
        capturing_ = false;
    }

    Args args_;
    std::shared_ptr<aditof::Camera> camera_;
    std::shared_ptr<aditof::DepthSensorInterface> sensor_;
    mutable std::mutex mu_;
    std::atomic<bool> capturing_{false};
    std::string mode_name_;
    std::string sensor_name_;
};

void send_status(Socket &sock, StatusCode code, const std::string &text, uint64_t frame_id = 0) {
    StatusPayload st;
    st.code = static_cast<uint16_t>(code);
    st.frame_id = frame_id;
    copy_cstr(st.text, sizeof(st.text), text);
    sock.send_message(MessageType::Status, &st, sizeof(st));
}

void session(const Args &args, CameraRawSource &cam) {
    Socket sock;
    std::cout << "Connecting to Machine B " << args.server_ip << ":" << args.port << " ...\n";
    sock.connect_to(args.server_ip, args.port, args.bind_ip);
    std::cout << "Connected. Waiting for commands.\n";

    HelloPayload hello;
    copy_cstr(hello.app_name, sizeof(hello.app_name), "tof_net_collect");
    copy_cstr(hello.sdk_version, sizeof(hello.sdk_version), aditof::getApiVersion());
    sock.send_message(MessageType::Hello, &hello, sizeof(hello));

    try {
        CameraRawSource::ModuleCcb ccb = cam.export_module_ccb();
        CcbFilePayloadHeader ccbHeader;
        ccbHeader.ccb_bytes = static_cast<uint32_t>(ccb.data.size());
        copy_cstr(ccbHeader.filename, sizeof(ccbHeader.filename), ccb.path);

        std::vector<uint8_t> ccbPayload(sizeof(ccbHeader) + ccb.data.size());
        std::memcpy(ccbPayload.data(), &ccbHeader, sizeof(ccbHeader));
        std::memcpy(ccbPayload.data() + sizeof(ccbHeader), ccb.data.data(),
                    ccb.data.size());

        sock.send_message(MessageType::CcbFile, ccbPayload.data(),
                          ccbPayload.size());

        std::ostringstream cs;
        cs << "sent current module CCB to Machine B: " << ccb.data.size()
           << " bytes from " << ccb.path;
        std::cout << cs.str() << "\n";
        send_status(sock, StatusCode::Ok, cs.str());
    } catch (const std::exception &e) {
        std::string text = std::string("failed to export/send current module CCB: ") + e.what();
        std::cerr << text << "\n";
        send_status(sock, StatusCode::Error, text);
    }

    // Send per-mode intrinsics + dealias data. ADSD3500 requires these for
    // InitTofiConfig_isp; they cannot be extracted from the CCB file alone.
    try {
        auto entries = cam.export_dealias_data();
        for (const auto &entry : entries) {
            DealiasDataPayload ddh;
            copy_cstr(ddh.frame_type, sizeof(ddh.frame_type), entry.frame_type);
            ddh.data_bytes = static_cast<uint32_t>(entry.data.size());

            std::vector<uint8_t> payload(sizeof(ddh) + entry.data.size());
            std::memcpy(payload.data(), &ddh, sizeof(ddh));
            std::memcpy(payload.data() + sizeof(ddh), entry.data.data(),
                        entry.data.size());

            sock.send_message(MessageType::DealiasData, payload.data(),
                              payload.size());

            std::ostringstream ds;
            ds << "sent dealias data for " << entry.frame_type
               << " (" << entry.data.size() << " bytes)";
            std::cout << ds.str() << "\n";
            send_status(sock, StatusCode::Ok, ds.str());
        }
    } catch (const std::exception &e) {
        std::string text =
            std::string("failed to export/send dealias data: ") + e.what();
        std::cerr << text << "\n";
        send_status(sock, StatusCode::Error, text);
    }

    send_status(sock, StatusCode::WaitingForCommand,
                "Machine A connected; CCB + dealias data sent; waiting for start command");

    std::atomic<bool> capture_thread_run{false};
    std::thread capture_thread;
    std::mutex send_mu;

    auto stop_thread = [&]() {
        capture_thread_run = false;
        cam.stop();
        if (capture_thread.joinable()) capture_thread.join();
    };

    try {
        while (g_run) {
            Message msg = sock.recv_message();
            if (msg.type == MessageType::StartCapture) {
                // Parse the requested mode from Machine B.
                std::string requestedFt;
                if (msg.payload.size() >= sizeof(StartCapturePayload)) {
                    StartCapturePayload sc;
                    std::memcpy(&sc, msg.payload.data(), sizeof(sc));
                    requestedFt = cstr_to_string(sc.frame_type, kNameLen);
                }
                stop_thread();
                if (!requestedFt.empty()) {
                    cam.switchMode(requestedFt);
                }
                cam.start();
                capture_thread_run = true;
                {
                    std::lock_guard<std::mutex> lock(send_mu);
                    send_status(sock, StatusCode::Capturing, "capture started; raw only; depth compute off");
                }
                capture_thread = std::thread([&]() {
                    uint64_t frame_id = 0;
                    const uint32_t max_frames = cam.max_frames();
                    while (g_run && capture_thread_run) {
                        if (max_frames != 0 && frame_id >= max_frames) {
                            capture_thread_run = false;
                            break;
                        }
                        try {
                            auto f = cam.get_frame();
                            auto compressed = zstd_compress(f.data.data(), f.data.size(), args.zstd_level);
                            if (frame_id == 0) {
                                std::cout << "[collect] First RAW frame:\n"
                                          << "[collect]   size=" << f.width << "x" << f.height
                                          << " raw_bytes=" << f.bytes
                                          << " compressed=" << compressed.size()
                                          << " ratio=" << (float)compressed.size() / std::max(1u, f.bytes)
                                          << "\n[collect]   subelem_size=" << f.subelement_size
                                          << " subelem_per_elem=" << f.subelements_per_element
                                          << " mode=" << f.mode
                                          << " frame_type=" << f.frame_type
                                          << "\n";
                            } else if (frame_id > 0 && frame_id % 30 == 0) {
                                std::cout << "[collect] Frame " << frame_id
                                          << ": raw=" << f.bytes
                                          << " compressed=" << compressed.size()
                                          << " ratio=" << (float)compressed.size() / std::max(1u, f.bytes)
                                          << "\n";
                            }

                            DataFramePayloadHeader ph;
                            ph.frame_id = ++frame_id;
                            ph.timestamp_ns = now_ns();
                            ph.raw_width = f.width;
                            ph.raw_height = f.height;
                            ph.raw_bytes = f.bytes;
                            ph.compressed_bytes = static_cast<uint32_t>(compressed.size());
                            ph.codec = static_cast<uint16_t>(Codec::Zstd);
                            ph.mode = f.mode;
                            copy_cstr(ph.frame_type, sizeof(ph.frame_type), f.frame_type);

                            std::vector<uint8_t> payload(sizeof(ph) + compressed.size());
                            std::memcpy(payload.data(), &ph, sizeof(ph));
                            std::memcpy(payload.data() + sizeof(ph), compressed.data(), compressed.size());

                            std::lock_guard<std::mutex> lock(send_mu);
                            sock.send_message(MessageType::DataFrame, payload.data(), payload.size());
                        } catch (const std::exception &e) {
                            try {
                                std::lock_guard<std::mutex> lock(send_mu);
                                send_status(sock, StatusCode::Error, e.what(), frame_id);
                            } catch (...) {}
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    }
                    try {
                        std::lock_guard<std::mutex> lock(send_mu);
                        send_status(sock, StatusCode::Stopped, "capture thread stopped", frame_id);
                    } catch (...) {}
                });
            } else if (msg.type == MessageType::StopCapture) {
                stop_thread();
                send_status(sock, StatusCode::Stopped, "capture stopped");
            } else if (msg.type == MessageType::Shutdown) {
                stop_thread();
                break;
            }
        }
        stop_thread();
    } catch (...) {
        stop_thread();
        throw;
    }
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    try {
        Args args = parse_args(argc, argv);
        CameraRawSource cam(args);
        cam.initialize();
        do {
            try {
                session(args, cam);
            } catch (const std::exception &e) {
                std::cerr << "session error: " << e.what() << "\n";
                cam.stop();
                if (!args.reconnect || !g_run) break;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } while (g_run && args.reconnect);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "fatal: " << e.what() << "\n";
        usage();
        return 1;
    }
}
