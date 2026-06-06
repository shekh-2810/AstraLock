#pragma once

#include <string>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace facelock {

using json = nlohmann::json;

// ============================================================
//  IPCServer — JSON-over-Unix-domain-socket server.
//
//  v3 changes vs v2.1:
//    - Thread pool replaces fork-per-connection.
//      n_threads workers handle concurrent auth + enroll requests
//      without the memory overhead or SIGCHLD complexity of forking.
//    - accept() loop runs on its own thread; start() returns immediately.
//    - stop() joins the accept thread and drains the pool cleanly.
//    - recv uses poll() with timeout so a slow/dead client cannot
//      block a worker thread indefinitely.
// ============================================================
class IPCServer {
public:
    using Handler = std::function<json(const json&)>;

    // socket_path : Unix socket path (e.g. /run/facelock/facelock.sock)
    // n_threads   : worker thread pool size (default 4)
    explicit IPCServer(const std::string& socket_path, int n_threads = 4);
    ~IPCServer();

    // Non-copyable
    IPCServer(const IPCServer&)            = delete;
    IPCServer& operator=(const IPCServer&) = delete;

    // Bind the socket, start the accept thread and thread pool.
    // handler is called on a pool thread for every incoming request.
    // Returns true on success.
    bool start(const Handler& handler);

    // Graceful shutdown: closes socket, joins accept thread, drains pool.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace facelock