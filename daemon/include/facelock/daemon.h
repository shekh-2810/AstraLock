#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace facelock {

using json = nlohmann::json;

struct DaemonConfig {
    std::string socket_path     = "/run/facelock/facelock.sock";
    std::string data_dir        = "/var/lib/facelock/";
    std::string onnx_model_path = "/usr/share/facelock/models/w600k_mbf.onnx";
    float       onnx_threshold  = 0.30f;
    int         camera_device   = 0;
};

class Daemon {
public:
    explicit Daemon(const DaemonConfig& cfg);
    int run();

private:
    DaemonConfig cfg_;
    bool initialize();
    json handle_request(const json& req);
};

} // namespace facelock