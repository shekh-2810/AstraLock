#include "facelock/daemon.h"
#include "facelock/ipc_server.h"
#include "facelock/onnx_wrapper.h"

#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <syslog.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

using namespace facelock;
namespace fs = std::filesystem;

// ============================================================
//  ONNX session cache — one instance per model path, shared
//  across all requests so we never pay session startup cost
//  during auth (was the biggest latency hit in v2.0).
// ============================================================
struct Daemon::Impl {
    std::unique_ptr<ONNXWrapper> onnx;
    std::mutex                   onnx_mtx;

    bool load(const std::string& model_path) {
        std::lock_guard<std::mutex> lk(onnx_mtx);
        if (onnx) return true;           // already loaded
        try {
            onnx = std::make_unique<ONNXWrapper>(model_path);
            // warmup: two dummy inferences so the first real auth isn't slow
            cv::Mat dummy(112, 112, CV_8UC3, cv::Scalar(128, 128, 128));
            onnx->warmup(dummy, 2);
            spdlog::info("ONNX session ready (model cached)");
            return true;
        } catch (const std::exception& e) {
            spdlog::error("ONNX session load failed: {}", e.what());
            return false;
        }
    }

    std::vector<float> embed(const cv::Mat& face) {
        std::lock_guard<std::mutex> lk(onnx_mtx);
        if (!onnx) return {};
        return onnx->embed(face);
    }
};

// ============================================================
//  Enrollment quality check — rejects blurry or too-small faces
// ============================================================
static bool quality_ok(const cv::Mat& bgr) {
    if (bgr.empty()) return false;

    // Laplacian variance — low = blurry
    cv::Mat gray, lap;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::Laplacian(gray, lap, CV_64F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(lap, mean, stddev);
    double sharpness = stddev.val[0] * stddev.val[0];

    if (sharpness < 30.0) {
        spdlog::debug("Quality reject: blurry (laplacian var={:.1f})", sharpness);
        return false;
    }

    // Rough brightness check: reject near-black or near-white frames
    cv::Scalar img_mean = cv::mean(gray);
    if (img_mean.val[0] < 20.0 || img_mean.val[0] > 240.0) {
        spdlog::debug("Quality reject: bad brightness (mean={:.1f})", img_mean.val[0]);
        return false;
    }

    return true;
}

// ============================================================
//  Camera capture
// ============================================================
static bool capture_face_bgr(cv::Mat& out, int camera_device = 0) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "/usr/lib/facelock/facelock-camera-helper --mode bgr112 --camera %d",
             camera_device);
    FILE* fp = popen(cmd, "r");
    if (!fp) return false;

    out = cv::Mat(112, 112, CV_8UC3);
    size_t need = 112 * 112 * 3;
    size_t got  = 0;
    auto start = std::chrono::steady_clock::now();

    while (got < need) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            pclose(fp);
            return false;
        }
        size_t r = fread(out.data + got, 1, need - got, fp);
        if (r == 0) break;
        got += r;
    }
    pclose(fp);
    return got == need;
}

// ============================================================
//  Audit log helper — writes structured line to syslog + spdlog
// ============================================================
static void audit(const std::string& event,
                  const std::string& user,
                  bool               ok,
                  float              score    = -1.f,
                  float              threshold = -1.f,
                  const std::string& detail   = "")
{
    std::string msg;
    if (score >= 0.f)
        msg = fmt::format("event={} user={} ok={} score={:.4f} threshold={:.4f}{}",
                          event, user, ok ? "true" : "false",
                          score, threshold,
                          detail.empty() ? "" : " detail=" + detail);
    else
        msg = fmt::format("event={} user={} ok={}{}",
                          event, user, ok ? "true" : "false",
                          detail.empty() ? "" : " detail=" + detail);

    if (ok) spdlog::info("[AUDIT] {}", msg);
    else    spdlog::warn("[AUDIT] {}", msg);

    // also emit to syslog for auditd / log aggregation
    openlog("facelockd", LOG_PID, LOG_AUTHPRIV);
    syslog(ok ? LOG_INFO : LOG_WARNING, "%s", msg.c_str());
    closelog();
}

// ============================================================
//  Daemon
// ============================================================
Daemon::Daemon(const DaemonConfig& cfg)
    : pimpl_(std::make_unique<Impl>()), cfg_(cfg) {}

Daemon::~Daemon() = default;

bool Daemon::initialize() {
    fs::create_directories(cfg_.data_dir);

    if (!fs::exists(cfg_.onnx_model_path)) {
        spdlog::error("ONNX model not found: {}", cfg_.onnx_model_path);
        spdlog::error("Run the installer or download the model to {}",
                      cfg_.onnx_model_path);
        return false;
    }

    if (!pimpl_->load(cfg_.onnx_model_path))
        return false;

    spdlog::info("AstraLock v2.1 daemon starting");
    spdlog::info("Model:     {}", cfg_.onnx_model_path);
    spdlog::info("Threshold: {:.4f}", cfg_.onnx_threshold);
    spdlog::info("Camera:    /dev/video{}", cfg_.camera_device);
    return true;
}

json Daemon::handle_request(const json& req) {
    const std::string cmd  = req.value("cmd",  "");
    const std::string user = req.value("user", "");

    if (user.empty())
        return {{"v",2},{"ok",false},{"err","no_user"},
                {"hint","Provide a 'user' field in the request"}};

    // ---- ENROLL ----
    if (cmd == "enroll") {
        fs::path userdir = fs::path(cfg_.data_dir) / user;
        fs::create_directories(userdir);

        std::vector<std::vector<float>> embeddings;
        int attempts     = 0;
        int quality_fails = 0;
        auto start = std::chrono::steady_clock::now();

        spdlog::info("Enroll started for user '{}'", user);

        while ((int)embeddings.size() < cfg_.enroll_target && attempts < 60) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(45)) {
                spdlog::warn("Enroll timeout for user '{}'", user);
                break;
            }

            cv::Mat face;
            if (!capture_face_bgr(face, cfg_.camera_device)) {
                ++attempts;
                continue;
            }

            if (!quality_ok(face)) {
                ++quality_fails;
                ++attempts;
                if (quality_fails % 5 == 0)
                    spdlog::info("Enroll '{}': {} quality rejects so far — "
                                 "ensure good lighting and face the camera",
                                 user, quality_fails);
                continue;
            }

            // save raw sample for potential re-training later
            cv::imwrite(
                (userdir / (std::to_string(embeddings.size()) + ".png")).string(),
                face
            );

            auto emb = pimpl_->embed(face);
            if (!emb.empty())
                embeddings.push_back(std::move(emb));

            ++attempts;
        }

        int got = (int)embeddings.size();

        if (got < cfg_.enroll_min) {
            audit("enroll", user, false, -1.f, -1.f,
                  fmt::format("only {} samples captured", got));
            return {{"v",2},{"ok",false},{"err","not_enough_faces"},{"got",got},
                    {"need",cfg_.enroll_min},
                    {"hint","Make sure the camera can see your face clearly, "
                            "lighting is adequate, and hold still during enrollment."}};
        }

        // write embedding file
        fs::path emb_path = fs::path(cfg_.data_dir) / (user + "_onnx_emb.bin");
        FILE* f = fopen(emb_path.c_str(), "wb");
        if (!f) {
            audit("enroll", user, false, -1.f, -1.f, "write_failed");
            return {{"v",2},{"ok",false},{"err","write_failed"},
                    {"hint","Check permissions on " + cfg_.data_dir}};
        }

        uint32_t N = (uint32_t)embeddings.size();
        uint32_t D = (uint32_t)embeddings[0].size();
        fwrite(&N, sizeof(N), 1, f);
        fwrite(&D, sizeof(D), 1, f);
        for (auto& e : embeddings)
            fwrite(e.data(), sizeof(float), D, f);
        fclose(f);

        audit("enroll", user, true, -1.f, -1.f,
              fmt::format("samples={} quality_rejects={}", N, quality_fails));
        spdlog::info("Enrolled {} embeddings for user '{}' ({} quality rejects)",
                     N, user, quality_fails);

        return {{"v",2},{"ok",true},{"samples",(int)N},{"quality_rejects",quality_fails}};
    }

    // ---- AUTH ----
    if (cmd == "auth") {
        fs::path emb_path = fs::path(cfg_.data_dir) / (user + "_onnx_emb.bin");
        if (!fs::exists(emb_path)) {
            audit("auth", user, false, -1.f, -1.f, "not_enrolled");
            return {{"v",2},{"ok",false},{"err","not_enrolled"},
                    {"hint","Run: facelock enroll " + user}};
        }

        FILE* f = fopen(emb_path.c_str(), "rb");
        if (!f) {
            audit("auth", user, false, -1.f, -1.f, "read_failed");
            return {{"v",2},{"ok",false},{"err","read_failed"}};
        }

        uint32_t N, D;
        fread(&N, sizeof(N), 1, f);
        fread(&D, sizeof(D), 1, f);
        std::vector<std::vector<float>> stored(N, std::vector<float>(D));
        for (auto& e : stored)
            fread(e.data(), sizeof(float), D, f);
        fclose(f);

        cv::Mat face;
        if (!capture_face_bgr(face, cfg_.camera_device)) {
            audit("auth", user, false, -1.f, cfg_.onnx_threshold, "no_face_detected");
            return {{"v",2},{"ok",false},{"err","no_face"},{"match",false},
                    {"hint","Position your face in front of the camera and try again"}};
        }

        auto query = pimpl_->embed(face);
        if (query.empty()) {
            audit("auth", user, false, -1.f, cfg_.onnx_threshold, "embed_failed");
            return {{"v",2},{"ok",false},{"err","embed_failed"},{"match",false}};
        }

        // top-3 cosine distance average for stability
        std::vector<float> dists;
        dists.reserve(N);
        for (auto& e : stored) {
            float dot = 0.f, na = 0.f, nb = 0.f;
            for (uint32_t i = 0; i < D; ++i) {
                dot += query[i] * e[i];
                na  += query[i] * query[i];
                nb  += e[i]     * e[i];
            }
            dists.push_back(1.0f - dot / (std::sqrt(na * nb) + 1e-12f));
        }
        std::sort(dists.begin(), dists.end());
        int top = std::min(3, (int)dists.size());
        float score = 0.f;
        for (int i = 0; i < top; ++i) score += dists[i];
        score /= top;

        bool match = score <= cfg_.onnx_threshold;
        audit("auth", user, match, score, cfg_.onnx_threshold);

        return {{"v",2},{"ok",true},{"match",match},{"score",score},{"err",nullptr}};
    }

    // ---- PING ----
    if (cmd == "ping")
        return {{"v",2},{"ok",true},{"pong",true}};

    return {{"v",2},{"ok",false},{"err","unknown_cmd"},
            {"hint","Valid commands: enroll, auth, ping"}};
}

int Daemon::run() {
    if (!initialize()) return 1;

    IPCServer server(cfg_.socket_path);
    server.start([this](const json& r) {
        return handle_request(r);
    });

    spdlog::info("Listening on {}", cfg_.socket_path);
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(60));
}
