// tof-loopsplat-collect — ADI ToF depth sequence recorder for LoopSplat
//
// Captures continuous depth frames with full camera metadata.
// Output: depth/ (16-bit PNG), depth.txt, camera.json, collect_log.txt
//
// Based on the acquisition model from examples/data_collect and
// the intrinsics export from examples/tof-net-collect.

#include <aditof/camera.h>
#include <aditof/camera_definitions.h>
#include <aditof/depth_sensor_interface.h>
#include <aditof/frame.h>
#include <aditof/frame_definitions.h>
#include <aditof/status_definitions.h>
#include <aditof/system.h>
#include <aditof/version.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#ifdef USE_GLOG
#include <glog/logging.h>
#else
#include <aditof/log.h>
#include <cstring>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t MAX_FILE_PATH = 512;
std::atomic<bool> g_run{true};

void on_signal(int) { g_run = false; }

struct Args {
    std::string config_json;   // ADI camera initialization JSON (positional)
    std::string camera_ip;     // --ip
    std::string firmware;      // --fw
    std::string output_dir = "./scene_001"; // --output
    uint16_t mode = 0;         // --m
    uint32_t n_frames = 0;     // --n  (0 = run until Ctrl+C)
    uint32_t warmup_time = 0;  // --wt
    uint32_t ext_fsync = 0;    // --ext_fsync
};

void usage() {
    std::cout << R"(tof-loopsplat-collect — LoopSplat depth sequence recorder

Usage:
  tof-loopsplat-collect [options] CONFIG_JSON

Arguments:
  CONFIG_JSON                  ADI camera initialization JSON
                               (e.g. config/config_crosby_adsd3500_new_modes.json)

Options:
  --ip <ip>                    Camera IP [default: USB enumeration]
  --m <mode>                   Camera mode number [default: 0]
                                 Mode 0: sr-native  (1024x1024)
                                 Mode 1: lr-native  (1024x1024)
                                 Mode 2: sr-qnative (512x512)
                                 Mode 3: lr-qnative (512x512)
                                 Mode 4: pcm-native (IR only, no depth!)
  --n <ncapture>               Number of frames to capture [default: 0 = unlimited]
  --output <dir>               Output directory [default: ./scene_001]
  --wt <warmup>                Warmup seconds before capture [default: 0]
  --fw <firmware>              Adsd3500 firmware file
  --ext_fsync <0|1>            External FSYNC [0: Internal, 1: External] [default: 0]
  -h, --help                   Show this help

Output structure:
  scene_001/
    depth/
      000000.png ...  (16-bit grayscale PNG, mm)
    depth.txt         (timestamp_ns depth/filename.png)
    camera.json       (intrinsics, distortion, depth_scale)
    collect_log.txt   (fps, drop count, valid depth ratio)
)";
}

Args parse_args(int argc, char **argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto need = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };
        if (k == "-h" || k == "--help") { usage(); std::exit(0); }
        else if (k == "--ip") a.camera_ip = need("--ip");
        else if (k == "--m") a.mode = static_cast<uint16_t>(std::stoi(need("--m")));
        else if (k == "--n") a.n_frames = static_cast<uint32_t>(std::stoul(need("--n")));
        else if (k == "--output") a.output_dir = need("--output");
        else if (k == "--wt") a.warmup_time = static_cast<uint32_t>(std::stoul(need("--wt")));
        else if (k == "--fw") a.firmware = need("--fw");
        else if (k == "--ext_fsync") a.ext_fsync = static_cast<uint32_t>(std::stoul(need("--ext_fsync")));
        else if (!k.empty() && k[0] != '-') {
            if (!a.config_json.empty()) throw std::runtime_error("multiple CONFIG_JSON arguments");
            a.config_json = k;
        } else {
            throw std::runtime_error("unknown option: " + k);
        }
    }
    if (a.config_json.empty()) {
        throw std::runtime_error("missing CONFIG_JSON argument");
    }
    return a;
}

bool file_exists(const std::string &path) {
    std::ifstream f(path);
    return static_cast<bool>(f);
}

std::string dirname_of(const std::string &path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
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
    // Try relative to executable directory
    if (argv0) {
        std::string alt = join_path(dirname_of(argv0), requested);
        if (file_exists(alt)) return alt;
    }
    return requested;
}

void ensure_dir(const std::string &path) {
    if (path.empty() || path == ".") return;
    // recursive mkdir
    std::string cur;
    size_t start = 0;
    if (path[0] == '/') { cur = "/"; start = 1; }
    while (start <= path.size()) {
        auto pos = path.find('/', start);
        std::string part = path.substr(start, (pos == std::string::npos) ? std::string::npos : pos - start);
        if (!part.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += part;
            mkdir(cur.c_str(), 0755); // ignore EEXIST
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
}

void write_camera_json(const std::string &path,
                       const aditof::IntrinsicParameters &in,
                       int width, int height,
                       const std::string &mode_name) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("cannot write " + path);

    f << std::fixed << std::setprecision(6);
    f << "{\n"
      << "  \"camera_model\": \"ADI_ADSD3500_Crosby\",\n"
      << "  \"mode\": \"" << mode_name << "\",\n"
      << "  \"depth_width\": " << width << ",\n"
      << "  \"depth_height\": " << height << ",\n"
      << "  \"depth_intrinsics\": {\n"
      << "    \"fx\": " << in.fx << ",\n"
      << "    \"fy\": " << in.fy << ",\n"
      << "    \"cx\": " << in.cx << ",\n"
      << "    \"cy\": " << in.cy << "\n"
      << "  },\n"
      << "  \"depth_scale\": 1000.0,\n"
      << "  \"depth_unit\": \"millimeter\",\n"
      << "  \"distortion\": ["
      << in.k1 << ", " << in.k2 << ", " << in.p1 << ", " << in.p2 << ", " << in.k3 << "]\n"
      << "}\n";
}

std::string frame_filename(int index) {
    std::ostringstream ss;
    ss << std::setw(6) << std::setfill('0') << index << ".png";
    return ss.str();
}

} // namespace

int main(int argc, char **argv) {
#ifdef USE_GLOG
    google::InitGoogleLogging(argv[0]);
    FLAGS_alsologtostderr = 1;
#endif
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    try {
        Args args = parse_args(argc, argv);
        args.config_json = resolve_config_path(args.config_json, argc > 0 ? argv[0] : nullptr);

        LOG(INFO) << "tof-loopsplat-collect starting";
        LOG(INFO) << "SDK version: " << aditof::getApiVersion()
                  << " | branch: " << aditof::getBranchVersion()
                  << " | commit: " << aditof::getCommitVersion();
        LOG(INFO) << "Config JSON: " << args.config_json;
        LOG(INFO) << "Mode: " << args.mode;
        LOG(INFO) << "Frames: " << (args.n_frames == 0 ? "unlimited" : std::to_string(args.n_frames));
        LOG(INFO) << "Output: " << args.output_dir;

        // ---- Camera initialization ----
        aditof::System system;
        std::vector<std::shared_ptr<aditof::Camera>> cameras;

        if (args.camera_ip.empty()) {
            system.getCameraList(cameras);
        } else {
            LOG(INFO) << "Camera IP: " << args.camera_ip;
            system.getCameraListAtIp(cameras, args.camera_ip);
        }

        if (cameras.empty()) {
            LOG(ERROR) << "No ADI ToF camera found";
            return 1;
        }

        auto camera = cameras.front();
        aditof::Status status;

        status = camera->setControl("initialization_config", args.config_json);
        if (status != aditof::Status::OK) {
            LOG(ERROR) << "setControl(initialization_config) failed";
            return 1;
        }

        status = camera->initialize();
        if (status != aditof::Status::OK) {
            LOG(ERROR) << "camera initialize failed";
            return 1;
        }

        // Firmware update
        if (!args.firmware.empty()) {
            status = camera->setControl("updateAdsd3500Firmware", args.firmware);
            if (status != aditof::Status::OK) {
                LOG(ERROR) << "Firmware update failed";
                return 1;
            }
            LOG(INFO) << "Firmware updated — reboot the board before capturing";
            return 0;
        }

        // Camera details (intrinsics)
        aditof::CameraDetails camDetails;
        camera->getDetails(camDetails);
        LOG(INFO) << "SD card image: " << camDetails.sdCardImageVersion;
        LOG(INFO) << "Kernel: " << camDetails.kernelVersion;
        LOG(INFO) << "U-Boot: " << camDetails.uBootVersion;

        // Resolve mode name
        std::string modeName;
        status = camera->getFrameTypeNameFromId(args.mode, modeName);
        if (status != aditof::Status::OK || modeName.empty()) {
            LOG(ERROR) << "Invalid mode: " << args.mode;
            return 1;
        }
        LOG(INFO) << "Mode name: " << modeName;

        // Check available frame types
        std::vector<std::string> frameTypes;
        camera->getAvailableFrameTypes(frameTypes);
        LOG(INFO) << "Available frame types: " << frameTypes.size();

        // Enable depth compute
        if (modeName == "pcm-native") {
            LOG(ERROR) << "pcm-native mode has no depth output. Use a different mode.";
            return 1;
        }
        status = camera->setControl("enableDepthCompute", "on");
        if (status != aditof::Status::OK) {
            LOG(ERROR) << "setControl(enableDepthCompute=on) failed";
            return 1;
        }

        status = camera->setFrameType(modeName);
        if (status != aditof::Status::OK) {
            LOG(ERROR) << "setFrameType failed: " << modeName;
            return 1;
        }

        // Update camera details after mode is set
        camera->getDetails(camDetails);
        int width = static_cast<int>(camDetails.frameType.width);
        int height = static_cast<int>(camDetails.frameType.height);
        auto &intr = camDetails.intrinsics;

        LOG(INFO) << "Resolution: " << width << "x" << height;
        LOG(INFO) << "Intrinsics — fx=" << intr.fx << " fy=" << intr.fy
                  << " cx=" << intr.cx << " cy=" << intr.cy;
        LOG(INFO) << "Distortion — k1=" << intr.k1 << " k2=" << intr.k2
                  << " k3=" << intr.k3 << " p1=" << intr.p1 << " p2=" << intr.p2;
        LOG(INFO) << "Depth range: " << camDetails.minDepth << "–" << camDetails.maxDepth << " mm";

        // ---- Output directory setup ----
        std::string depth_dir = join_path(args.output_dir, "depth");
        ensure_dir(depth_dir);

        // Write camera.json
        std::string camera_json_path = join_path(args.output_dir, "camera.json");
        write_camera_json(camera_json_path, intr, width, height, modeName);
        LOG(INFO) << "Wrote " << camera_json_path;

        // Open depth.txt for appending
        std::string depth_txt_path = join_path(args.output_dir, "depth.txt");
        std::ofstream depth_txt(depth_txt_path, std::ios::out | std::ios::trunc);
        if (!depth_txt) {
            LOG(ERROR) << "Cannot open " << depth_txt_path;
            return 1;
        }
        depth_txt << "# timestamp_ns depth_file\n";
        depth_txt << "# depth_scale = 1000 (value / 1000 = meters)\n";
        depth_txt << "# depth_unit = millimeter\n";

        // Open log
        std::string log_path = join_path(args.output_dir, "collect_log.txt");
        std::ofstream log_file(log_path, std::ios::out | std::ios::trunc);

        // ---- Start camera ----
        status = camera->start();
        if (status != aditof::Status::OK) {
            LOG(ERROR) << "camera start failed";
            return 1;
        }

        // Sync mode
        if (args.ext_fsync == 0) {
            camera->setControl("syncMode", "0, 0");
        } else if (args.ext_fsync == 1) {
            camera->setControl("syncMode", "2, 0");
        }

        // ---- Warmup ----
        if (args.warmup_time > 0) {
            LOG(INFO) << "Warmup for " << args.warmup_time << " seconds...";
            auto warmup_start = std::chrono::steady_clock::now();
            while (g_run) {
                aditof::Frame dummy;
                camera->requestFrame(&dummy);
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - warmup_start).count();
                if (elapsed >= static_cast<long>(args.warmup_time)) break;
            }
        }

        // Drop first frame after start (may contain stale data)
        {
            aditof::Frame dummy;
            camera->requestFrame(&dummy);
            LOG(INFO) << "Dropped first frame after camera start";
        }

        // ---- Main capture loop ----
        LOG(INFO) << "Starting capture..."
                  << (args.n_frames == 0 ? " (Ctrl+C to stop)" : "");

        auto capture_start = std::chrono::steady_clock::now();
        uint32_t frame_count = 0;
        uint32_t drop_count = 0;
        uint64_t total_valid_pixels = 0;
        uint64_t total_pixels = 0;

        while (g_run) {
            if (args.n_frames > 0 && frame_count >= args.n_frames) {
                break;
            }

            aditof::Frame frame;
            status = camera->requestFrame(&frame);
            if (status != aditof::Status::OK) {
                LOG(ERROR) << "requestFrame failed at frame " << frame_count;
                drop_count++;
                continue;
            }

            uint16_t *depthData = nullptr;
            status = frame.getData("depth", &depthData);
            if (status != aditof::Status::OK || !depthData) {
                LOG(ERROR) << "getData(depth) failed at frame " << frame_count;
                drop_count++;
                continue;
            }

            // Timestamp
            auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // Write 16-bit PNG via OpenCV
            std::string fname = frame_filename(frame_count);
            std::string png_path = join_path(depth_dir, fname);

            cv::Mat depth_mat(height, width, CV_16UC1, depthData);
            if (!cv::imwrite(png_path, depth_mat)) {
                LOG(ERROR) << "Failed to write " << png_path;
                drop_count++;
                continue;
            }

            // Append depth.txt
            depth_txt << now_ns << " depth/" << fname << "\n";
            if (frame_count % 10 == 0) depth_txt.flush();

            // Statistics
            uint64_t valid = 0;
            uint64_t total = static_cast<uint64_t>(width) * height;
            for (size_t i = 0; i < total; ++i) {
                if (depthData[i] > 0) valid++;
            }
            total_valid_pixels += valid;
            total_pixels += total;

            frame_count++;

            // Progress log every 30 frames
            if (frame_count == 1 || frame_count % 30 == 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - capture_start).count();
                double fps = elapsed > 0 ? static_cast<double>(frame_count) / elapsed : 0;
                double valid_pct = total_pixels > 0
                    ? 100.0 * static_cast<double>(total_valid_pixels) / static_cast<double>(total_pixels)
                    : 0;
                LOG(INFO) << "Frame " << frame_count
                          << " | fps=" << std::fixed << std::setprecision(1) << fps
                          << " | valid_depth=" << std::setprecision(1) << valid_pct << "%"
                          << " | drops=" << drop_count;
            }
        }

        // ---- Stop & finalize ----
        status = camera->stop();
        if (status != aditof::Status::OK) {
            LOG(WARNING) << "camera stop returned non-OK";
        }
        auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - capture_start).count();
        double avg_fps = total_elapsed > 0
            ? static_cast<double>(frame_count) / total_elapsed
            : 0;
        double valid_pct = total_pixels > 0
            ? 100.0 * static_cast<double>(total_valid_pixels) / static_cast<double>(total_pixels)
            : 0;

        depth_txt.close();
        log_file << "frames_captured: " << frame_count << "\n"
                 << "frames_dropped: " << drop_count << "\n"
                 << "total_duration_s: " << total_elapsed << "\n"
                 << "average_fps: " << avg_fps << "\n"
                 << "valid_depth_pct: " << valid_pct << "\n"
                 << "mode: " << modeName << "\n"
                 << "resolution: " << width << "x" << height << "\n"
                 << "depth_scale: 1000\n"
                 << "depth_unit: millimeter\n";
        log_file.close();

        LOG(INFO) << "=== Capture complete ===";
        LOG(INFO) << "Frames: " << frame_count
                  << " | Drops: " << drop_count
                  << " | Duration: " << total_elapsed << "s"
                  << " | Avg FPS: " << avg_fps
                  << " | Valid depth: " << valid_pct << "%";
        LOG(INFO) << "Output: " << args.output_dir;

    } catch (const std::exception &e) {
        LOG(ERROR) << "Fatal: " << e.what();
        return 1;
    }

    return 0;
}
