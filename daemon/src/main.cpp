#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>

#include "facelock/daemon.h"
#include "facelock/version.h"
#include <spdlog/spdlog.h>

#include <fstream>
#include <string>
#include <filesystem>
#include <systemd/sd-daemon.h>

namespace fs = std::filesystem;

// ============================================================
//  Config parser — /etc/facelock/facelock.conf
//
//  Format: KEY=VALUE, # comments, blank lines ignored.
//  Any key not present keeps its compiled-in default from DaemonConfig.
//  Parse errors (bad float/int) log a warning and keep the default
//  rather than crashing the daemon.
// ============================================================
static facelock::DaemonConfig load_config(const std::string& path)
{
    facelock::DaemonConfig cfg;  // compiled-in defaults

    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("Config '{}' not found — using compiled defaults", path);
        return cfg;
    }

    auto safe_float = [](const std::string& v, float def,
                         const std::string& key) -> float {
        try { return std::stof(v); }
        catch (...) {
            spdlog::warn("Config: invalid float for {} ('{}') — using default {:.4f}",
                         key, v, def);
            return def;
        }
    };

    auto safe_int = [](const std::string& v, int def,
                       const std::string& key) -> int {
        try { return std::stoi(v); }
        catch (...) {
            spdlog::warn("Config: invalid int for {} ('{}') — using default {}",
                         key, v, def);
            return def;
        }
    };

    auto safe_bool = [](const std::string& v, bool def,
                        const std::string& key) -> bool {
        if (v == "true"  || v == "1" || v == "yes") return true;
        if (v == "false" || v == "0" || v == "no")  return false;
        spdlog::warn("Config: invalid bool for {} ('{}') — using default {}",
                     key, v, def ? "true" : "false");
        return def;
    };

    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;

        // Strip comments
        auto pos = line.find('#');
        if (pos != std::string::npos) line = line.substr(0, pos);

        // Strip trailing whitespace / CR
        while (!line.empty() &&
               (line.back() == ' ' || line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) {
            spdlog::warn("Config line {}: no '=' found, skipping: '{}'", lineno, line);
            continue;
        }

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        auto trim = [](std::string& s) {
            while (!s.empty() && s.front() == ' ') s.erase(s.begin());
            while (!s.empty() && s.back()  == ' ') s.pop_back();
        };
        trim(key); trim(value);

        // --- IPC ---
        if      (key == "SOCKET_PATH")          cfg.socket_path          = value;
        // --- Storage ---
        else if (key == "DATA_DIR")             cfg.data_dir             = value;
        // --- Models ---
        else if (key == "ONNX_MODEL_PATH")      cfg.onnx_model_path      = value;
        else if (key == "DETECTOR_MODEL_PATH")  cfg.detector_model_path  = value;
        else if (key == "DETECTOR_CONFIDENCE")
            cfg.detector_confidence = safe_float(value, cfg.detector_confidence, key);
        else if (key == "DETECTOR_NMS")
            cfg.detector_nms = safe_float(value, cfg.detector_nms, key);
        // --- Scoring ---
        else if (key == "ONNX_THRESHOLD")
            cfg.onnx_threshold = safe_float(value, cfg.onnx_threshold, key);
        // --- Camera ---
        else if (key == "CAMERA_DEVICE")
            cfg.camera_device = safe_int(value, cfg.camera_device, key);
        // --- Enrollment ---
        else if (key == "ENROLL_TARGET")
            cfg.enroll_target = safe_int(value, cfg.enroll_target, key);
        else if (key == "ENROLL_MIN")
            cfg.enroll_min = safe_int(value, cfg.enroll_min, key);
        else if (key == "ENROLL_THRESHOLD_K")
            cfg.enroll_threshold_k = safe_float(value, cfg.enroll_threshold_k, key);
        // --- Liveness ---
        else if (key == "LIVENESS_ENABLED")
            cfg.liveness_enabled = safe_bool(value, cfg.liveness_enabled, key);
        else if (key == "LIVENESS_TEXTURE_MIN")
            cfg.liveness_texture_min = safe_float(value, cfg.liveness_texture_min, key);
        else if (key == "LIVENESS_BLINK_FRAMES")
            cfg.liveness_blink_frames = safe_int(value, cfg.liveness_blink_frames, key);
        else if (key == "LIVENESS_EAR_THRESHOLD")
            cfg.liveness_ear_threshold = safe_float(value, cfg.liveness_ear_threshold, key);
        // --- Thread pool ---
        else if (key == "IPC_THREADS")
            cfg.ipc_threads = safe_int(value, cfg.ipc_threads, key);
        // --- Startup ---
        else if (key == "CAMERA_WARMUP_FRAMES")
            cfg.camera_warmup_frames = safe_int(value, cfg.camera_warmup_frames, key);
        else if (key == "READY_FILE")
            cfg.ready_file = value;
        else
            spdlog::warn("Config line {}: unknown key '{}' — ignored", lineno, key);
    }

    spdlog::info("Config loaded from '{}'", path);
    return cfg;
}

// ============================================================
//  main
// ============================================================
int main(int argc, char** argv)
{
    // Hard-disable OpenCL/GPU paths — prevents OCL init crashes on headless
    // systems and keeps inference deterministic.
    cv::ocl::setUseOpenCL(false);
    cv::setUseOptimized(false);
    cv::setNumThreads(1);

    spdlog::set_level(spdlog::level::info);
    spdlog::info("AstraLock {} starting up", FACELOCK_VERSION_STRING);

    const char* config_path = "/etc/facelock/facelock.conf";
    if (argc >= 2) config_path = argv[1];

    facelock::DaemonConfig cfg = load_config(config_path);

    spdlog::info("camera_device={}  threshold={:.4f}  liveness={}  threads={}",
                 cfg.camera_device,
                 cfg.onnx_threshold,
                 cfg.liveness_enabled ? "on" : "off",
                 cfg.ipc_threads);

    facelock::Daemon daemon(cfg);
    int rc = daemon.run();

    // If run() returns it means initialization failed before the socket was
    // bound.  Notify systemd of failure so it doesn't wait for READY=1.
    if (rc != 0)
        sd_notify(0, "STATUS=initialization failed\nERRNO=1");

    return rc;
}
