#pragma once
#include <string>
#include <memory>
#include <nlohmann/json.hpp>

namespace facelock {

using json = nlohmann::json;

struct DaemonConfig {
    std::string socket_path     = "/run/facelock/facelock.sock";
    std::string data_dir        = "/var/lib/facelock/";
    std::string onnx_model_path = "/usr/share/facelock/models/w600k_mbf.onnx";
    float       onnx_threshold  = 0.30f;
    int         camera_device   = 0;
    int         enroll_target   = 20;   // desired number of enrollment samples
    int         enroll_min      = 10;   // minimum accepted
};

class Daemon {
public:
    explicit Daemon(const DaemonConfig& cfg);
    ~Daemon();
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

    DaemonConfig cfg_;
    bool initialize();
    json handle_request(const json& req);
};

} // namespace facelock
