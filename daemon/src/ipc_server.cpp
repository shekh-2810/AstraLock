#include "facelock/ipc_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

#include <thread>
#include <cstring>
#include <filesystem>
#include <sys/wait.h>

using namespace facelock;
namespace fs = std::filesystem;

/* ===================== ctor / dtor ===================== */

IPCServer::IPCServer(const std::string& socket_path)
    : socket_path_(socket_path),
      server_fd_(-1),
      running_(false) {}

IPCServer::~IPCServer() {
    stop();
}

/* ===================== lifecycle ===================== */

bool IPCServer::start(const Handler& handler) {
    if (running_)
        return false;

    fs::path p(socket_path_);
    if (p.has_parent_path())
        fs::create_directories(p.parent_path());

    ::unlink(socket_path_.c_str());

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0)
        return false;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); // auto-reap children

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path,
                 socket_path_.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        close(server_fd_);
        return false;
    }

    chmod(socket_path_.c_str(), 0660);

    if (listen(server_fd_, 16) < 0) {
        close(server_fd_);
        ::unlink(socket_path_.c_str());
        return false;
    }

    running_ = true;
    std::thread(&IPCServer::accept_loop, this, handler).detach();
    return true;
}

void IPCServer::stop() {
    running_ = false;

    if (server_fd_ >= 0)
        close(server_fd_);

    ::unlink(socket_path_.c_str());
}

/* ===================== accept loop ===================== */

void IPCServer::accept_loop(Handler handler) {
    while (running_) {
        int client = accept(server_fd_, nullptr, nullptr);
        if (client < 0)
            continue;

        pid_t pid = fork();
        if (pid < 0) {
            close(client);
            continue;
        }

        if (pid == 0) {
            // ---- CHILD ----
            close(server_fd_);

            std::string data;
            char buf[1024];

            while (true) {
                ssize_t r = read(client, buf, sizeof(buf));
                if (r <= 0)
                    break;
                data.append(buf, (size_t)r);
                if (data.find('\n') != std::string::npos)
                    break;
            }

            nlohmann::json resp;
            try {
                auto req = nlohmann::json::parse(data);
                resp = handler(req);
            } catch (const std::exception& e) {
                resp = {
                    {"ok", false},
                    {"err", std::string("parse_error: ") + e.what()}
                };
            }

            std::string out = resp.dump();
            out.push_back('\n');

            ssize_t total = 0;
            while (total < (ssize_t)out.size()) {
                ssize_t w = send(client,
                                 out.data() + total,
                                 out.size() - total,
                                 MSG_NOSIGNAL);
                if (w <= 0)
                    break;
                total += w;
            }

            close(client);
            _exit(0);
        }

        // ---- PARENT ----
        close(client);
    }
}
