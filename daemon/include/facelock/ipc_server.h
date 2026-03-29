#pragma once
#include <string>
#include <functional>
#include <nlohmann/json.hpp>

namespace facelock {

using json = nlohmann::json;

// Simple JSON-over-UDS server. Accepts single JSON request per connection and returns JSON.
class IPCServer {
public:
    using Handler = std::function<json(const json&)>;

    explicit IPCServer(const std::string& socket_path);
    ~IPCServer();

    // start listening and spawn accept thread. returns true on success.
    bool start(const Handler& handler);
    void stop();

private:
    std::string socket_path_;
    int server_fd_ = -1;
    bool running_ = false;

    // accept loop runs in a detached thread
    void accept_loop(Handler handler);

    // helpers
    static std::string trim(const std::string &s);
};

} // namespace facelock
