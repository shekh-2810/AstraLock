#include "facelock/daemon.h"
#include "facelock/ipc_server.h"
#include "facelock/onnx_wrapper.h"

#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>

using namespace facelock;
namespace fs = std::filesystem;

Daemon::Daemon(const DaemonConfig& cfg) : cfg_(cfg) {}

bool Daemon::initialize() {
    fs::create_directories(cfg_.data_dir);
    if (!fs::exists(cfg_.onnx_model_path)) {
        spdlog::error("ONNX model not found: {}", cfg_.onnx_model_path);
        return false;
    }
    spdlog::info("AstraLock v2.0 daemon starting");
    spdlog::info("Model: {}", cfg_.onnx_model_path);
    spdlog::info("Threshold: {:.4f}", cfg_.onnx_threshold);
    return true;
}

static bool capture_face_bgr(cv::Mat& out) {
    FILE* fp = popen("/usr/lib/facelock/facelock-camera-helper --mode bgr112", "r");
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

json Daemon::handle_request(const json& req) {
    const std::string cmd  = req.value("cmd",  "");
    const std::string user = req.value("user", "");

    if (user.empty())
        return {{"v",2},{"ok",false},{"err","no_user"}};

    /* ---------- ENROLL ---------- */
    if (cmd == "enroll") {
        fs::path userdir = fs::path(cfg_.data_dir) / user;
        fs::create_directories(userdir);

        ONNXWrapper onnx(cfg_.onnx_model_path);
        std::vector<std::vector<float>> embeddings;
        int attempts = 0;
        auto start = std::chrono::steady_clock::now();

        while ((int)embeddings.size() < 20 && attempts < 50) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(40))
                break;

            cv::Mat face;
            if (!capture_face_bgr(face)) { ++attempts; continue; }

            cv::imwrite(
                (userdir / (std::to_string(embeddings.size()) + ".png")).string(),
                face
            );

            auto emb = onnx.embed(face);
            if (!emb.empty())
                embeddings.push_back(std::move(emb));

            ++attempts;
        }

        if ((int)embeddings.size() < 10)
            return {{"v",2},{"ok",false},{"err","not_enough_faces"},{"got",(int)embeddings.size()}};

        fs::path emb_path = fs::path(cfg_.data_dir) / (user + "_onnx_emb.bin");
        FILE* f = fopen(emb_path.c_str(), "wb");
        if (!f)
            return {{"v",2},{"ok",false},{"err","write_failed"}};

        uint32_t N = (uint32_t)embeddings.size();
        uint32_t D = (uint32_t)embeddings[0].size();
        fwrite(&N, sizeof(N), 1, f);
        fwrite(&D, sizeof(D), 1, f);
        for (auto& e : embeddings)
            fwrite(e.data(), sizeof(float), D, f);
        fclose(f);

        spdlog::info("Enrolled {} embeddings for user '{}'", N, user);
        return {{"v",2},{"ok",true},{"samples",(int)N}};
    }

    /* ---------- AUTH ---------- */
    if (cmd == "auth") {
        fs::path emb_path = fs::path(cfg_.data_dir) / (user + "_onnx_emb.bin");
        if (!fs::exists(emb_path))
            return {{"v",2},{"ok",false},{"err","not_enrolled"}};

        FILE* f = fopen(emb_path.c_str(), "rb");
        if (!f)
            return {{"v",2},{"ok",false},{"err","model_load_failed"}};

        uint32_t N, D;
        fread(&N, sizeof(N), 1, f);
        fread(&D, sizeof(D), 1, f);

        std::vector<std::vector<float>> stored(N, std::vector<float>(D));
        for (auto& e : stored)
            fread(e.data(), sizeof(float), D, f);
        fclose(f);

        cv::Mat face;
        if (!capture_face_bgr(face))
            return {{"v",2},{"ok",false},{"err","no_face"},{"match",false}};

        ONNXWrapper onnx(cfg_.onnx_model_path);
        auto query = onnx.embed(face);
        if (query.empty())
            return {{"v",2},{"ok",false},{"err","model_load_failed"}};

        // collect all cosine distances, average top-3 for stability
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
        float best = 0.f;
        for (int i = 0; i < top; ++i) best += dists[i];
        best /= top;

        bool match = best <= cfg_.onnx_threshold;
        spdlog::info("Auth '{}': score={:.4f} threshold={:.4f} match={}",
                     user, best, cfg_.onnx_threshold, match);

        return {{"v",2},{"ok",true},{"match",match},{"score",best},{"err",nullptr}};
    }

    /* ---------- PING ---------- */
    if (cmd == "ping")
        return {{"v",2},{"ok",true},{"pong",true}};

    return {{"v",2},{"ok",false},{"err","unknown_cmd"}};
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