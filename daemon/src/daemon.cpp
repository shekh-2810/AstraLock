#include "facelock/daemon.h"
#include "facelock/ipc_server.h"
#include "facelock/lbph_wrapper.h"

#include <filesystem>
#include <thread>
#include <cstdio>
#include <opencv2/core.hpp>

using namespace facelock;
namespace fs = std::filesystem;

/* ---------------- ctor ---------------- */

Daemon::Daemon(const DaemonConfig& cfg) : cfg_(cfg) {}

bool Daemon::initialize() {
    fs::create_directories(cfg_.data_dir);
    return true;
}

/* -------- SAFE face capture (NON-BLOCKING) -------- */

static bool capture_one_face(cv::Mat &out) {
    FILE* fp = popen("/usr/lib/facelock/facelock-camera-helper", "r");
    if (!fp)
        return false;

    out = cv::Mat(200, 200, CV_8UC1);

    size_t need = out.total();
    size_t got  = 0;

    auto start = std::chrono::steady_clock::now();

    while (got < need) {
        if (std::chrono::steady_clock::now() - start >
            std::chrono::seconds(2)) {
            pclose(fp);
            return false;
        }

        size_t r = fread(out.data + got, 1, need - got, fp);
        if (r == 0)
            break;
        got += r;
    }

    pclose(fp);
    return got == need;
}


/* ---------------- IPC handler ---------------- */

json Daemon::handle_request(const json& req) {
    const std::string cmd  = req.value("cmd", "");
    const std::string user = req.value("user", "");

    if (user.empty())
        return {{"ok", false}, {"err", "no_user"}};

    /* ---------- ENROLL ---------- */
    if (cmd == "enroll") {
        fs::path userdir = fs::path(cfg_.data_dir) / user;
        fs::create_directories(userdir);

        int saved = 0;
        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < 30 && saved < 8; ++i) {
            if (std::chrono::steady_clock::now() - start >
                std::chrono::seconds(12))
                break;

            cv::Mat face;
            if (!capture_one_face(face))
                continue;

            cv::imwrite(
                (userdir / (std::to_string(saved) + ".png")).string(),
                face
            );
            ++saved;
        }

        if (saved < 5)
            return {{"ok", false}, {"err", "not_enough_faces"}, {"got", saved}};

        return {{"ok", true}, {"samples", saved}};
    }

    /* ---------- AUTH ---------- */
    if (cmd == "auth") {
        cv::Mat face;
        if (!capture_one_face(face))
            return {{"ok", false}, {"err", "no_face"}};

        LBPHWrapper lbph;
        if (!lbph.load(user, cfg_.data_dir))
            return {{"ok", false}, {"err", "lbph_load_failed"}};

        auto r = lbph.predict(face, cfg_.lbph_threshold);
        if (r && r->match)
            return {{"ok", true}, {"match", true}};

        return {{"ok", true}, {"match", false}};
    }

    return {{"ok", false}, {"err", "unknown_cmd"}};
}

/* ---------------- main loop ---------------- */

int Daemon::run() {
    if (!initialize())
        return 1;

    IPCServer server(cfg_.socket_path);

    server.start([this](const json& r) {
        return handle_request(r);
    });

    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(60));
}
