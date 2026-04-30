#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>

#include "facelock/daemon.h"
#include <spdlog/spdlog.h>

#include <fstream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Parse /etc/facelock/facelock.conf  (KEY=VALUE, # comments, blank lines ok)
// ---------------------------------------------------------------------------
static facelock::DaemonConfig load_config(const std::string &path) {
    facelock::DaemonConfig cfg; // compiled-in defaults

    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("Config file not found ({}), using defaults", path);
        return cfg;
    }

    std::string line;
    while (std::getline(f, line)) {
        // strip comments
        auto pos = line.find('#');
        if (pos != std::string::npos) line = line.substr(0, pos);

        // strip trailing whitespace / CR
        while (!line.empty() &&
               (line.back() == ' ' || line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // trim both sides
        auto trim = [](std::string &s) {
            while (!s.empty() && s.front() == ' ') s.erase(s.begin());
            while (!s.empty() && s.back()  == ' ') s.pop_back();
        };
        trim(key); trim(value);

        if      (key == "SOCKET_PATH")     cfg.socket_path     = value;
        else if (key == "DATA_DIR")        cfg.data_dir        = value;
        else if (key == "ONNX_MODEL_PATH") cfg.onnx_model_path = value;
        else if (key == "ONNX_THRESHOLD")  cfg.onnx_threshold  = std::stof(value);
        else if (key == "CAMERA_DEVICE")   cfg.camera_device   = std::stoi(value);
    }

    spdlog::info("Config loaded from {}", path);
    return cfg;
}

int main(int argc, char** argv) {
    // Hard-disable OpenCL / GPU paths (prevents OCL crashes)
    cv::ocl::setUseOpenCL(false);
    cv::setUseOptimized(false);
    cv::setNumThreads(1);

    spdlog::set_level(spdlog::level::info);

    facelock::DaemonConfig cfg = load_config("/etc/facelock/facelock.conf");

    spdlog::info("camera_device={} threshold={:.3f}",
                 cfg.camera_device, cfg.onnx_threshold);

    facelock::Daemon daemon(cfg);
    return daemon.run();
}
