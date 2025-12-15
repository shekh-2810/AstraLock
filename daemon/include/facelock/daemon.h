#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace facelock {

using json = nlohmann::json;

struct DaemonConfig {
    std::string socket_path = "/var/run/facelock/facelock.sock";
    std::string data_dir = "/var/lib/facelock/";
    int camera_device = 0;
    float lbph_threshold = 0.09f;
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

}
