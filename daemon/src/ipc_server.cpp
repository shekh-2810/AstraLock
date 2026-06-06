#include "facelock/ipc_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>

#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <spdlog/spdlog.h>

using namespace facelock;
namespace fs = std::filesystem;

// ============================================================
//  ThreadPool — fixed-size worker pool for IPC connections
// ============================================================
class ThreadPool {
public:
    explicit ThreadPool(int n_threads) : stop_(false) {
        for (int i = 0; i < n_threads; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) return;
            queue_.push(std::move(task));
        }
        cv_.notify_one();
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }
            task();
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex                        mtx_;
    std::condition_variable           cv_;
    bool                              stop_;
};

// ============================================================
//  IPCServer::Impl
// ============================================================
struct IPCServer::Impl {
    std::string  socket_path;
    int          n_threads;
    int          server_fd = -1;
    std::atomic<bool> running{false};
    std::unique_ptr<ThreadPool> pool;
    std::thread  accept_thread;
};

// ============================================================
//  Connection handler — runs on a pool thread
//
//  Read one newline-terminated JSON request, parse it, call
//  handler, write JSON response + newline.
//
//  recv_all / send_all handle partial reads/writes correctly.
// ============================================================
static std::string recv_line(int fd, int timeout_ms = 8000)
{
    std::string buf;
    buf.reserve(512);

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);

    while (true) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;

        int rc = poll(&pfd, 1, static_cast<int>(remaining));
        if (rc <= 0) break;  // timeout or error

        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) break;  // EOF or error

        buf.push_back(c);
        if (c == '\n') break;
        if (buf.size() > 4096) break;  // request too large — reject
    }
    return buf;
}

static void handle_connection(int client_fd, const IPCServer::Handler& handler)
{
    // Ensure socket is closed when this scope exits
    struct Guard { int fd; ~Guard() { if (fd >= 0) ::close(fd); } } guard{client_fd};

    std::string raw = recv_line(client_fd);
    if (raw.empty()) return;

    nlohmann::json resp;
    try {
        auto req = nlohmann::json::parse(raw);

        int ver = req.value("v", 1);
        if (ver > 3) {
            resp = {{"v", 3}, {"ok", false}, {"err", "unsupported_version"},
                    {"hint", "Client is newer than daemon. Update AstraLock."}};
        } else {
            resp = handler(req);
        }
    } catch (const std::exception& e) {
        spdlog::warn("IPC parse error: {}", e.what());
        resp = {{"v", 3}, {"ok", false}, {"err", "parse_error"},
                {"hint", "Request must be valid JSON terminated with \\n"}};
    }

    std::string out = resp.dump();
    out.push_back('\n');

    // send_all — handles partial writes
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(out.size())) {
        ssize_t w = send(client_fd, out.data() + total,
                         out.size() - static_cast<size_t>(total), MSG_NOSIGNAL);
        if (w <= 0) break;
        total += w;
    }
}

// ============================================================
//  IPCServer public interface
// ============================================================
IPCServer::IPCServer(const std::string& socket_path, int n_threads)
    : pimpl_(std::make_unique<Impl>())
{
    pimpl_->socket_path = socket_path;
    pimpl_->n_threads   = std::max(1, n_threads);
}

IPCServer::~IPCServer() { stop(); }

bool IPCServer::start(const Handler& handler)
{
    if (pimpl_->running) return false;

    signal(SIGPIPE, SIG_IGN);

    // Create socket directory
    fs::path p(pimpl_->socket_path);
    if (p.has_parent_path())
        fs::create_directories(p.parent_path());

    ::unlink(pimpl_->socket_path.c_str());

    pimpl_->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (pimpl_->server_fd < 0) {
        spdlog::error("IPC: socket() failed: {}", strerror(errno));
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, pimpl_->socket_path.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (bind(pimpl_->server_fd,
             reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("IPC: bind() failed: {}", strerror(errno));
        close(pimpl_->server_fd);
        pimpl_->server_fd = -1;
        return false;
    }

    // 0666 so PAM module (running as root but dropping to user context)
    // and the facelock CLI (running as the enrolled user) can both connect.
    chmod(pimpl_->socket_path.c_str(), 0666);

    if (listen(pimpl_->server_fd, 32) < 0) {
        spdlog::error("IPC: listen() failed: {}", strerror(errno));
        close(pimpl_->server_fd);
        ::unlink(pimpl_->socket_path.c_str());
        pimpl_->server_fd = -1;
        return false;
    }

    pimpl_->pool    = std::make_unique<ThreadPool>(pimpl_->n_threads);
    pimpl_->running = true;

    // Accept loop runs on its own thread so start() returns immediately.
    pimpl_->accept_thread = std::thread([this, handler]() {
        while (pimpl_->running) {
            struct pollfd pfd = { pimpl_->server_fd, POLLIN, 0 };
            if (poll(&pfd, 1, 500) <= 0) continue;  // 500ms tick for clean shutdown

            int client = accept(pimpl_->server_fd, nullptr, nullptr);
            if (client < 0) {
                if (pimpl_->running)
                    spdlog::warn("IPC: accept() failed: {}", strerror(errno));
                continue;
            }

            // Hand off to thread pool — zero copies of handler (shared_ptr semantics)
            pimpl_->pool->enqueue([client, &handler]() {
                handle_connection(client, handler);
            });
        }
    });

    return true;
}

void IPCServer::stop()
{
    if (!pimpl_->running.exchange(false)) return;

    if (pimpl_->server_fd >= 0) {
        close(pimpl_->server_fd);
        pimpl_->server_fd = -1;
    }
    ::unlink(pimpl_->socket_path.c_str());

    if (pimpl_->accept_thread.joinable())
        pimpl_->accept_thread.join();

    pimpl_->pool.reset();  // joins all worker threads
}