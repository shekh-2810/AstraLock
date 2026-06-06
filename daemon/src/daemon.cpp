#include "facelock/daemon.h"
#include "facelock/camera.h"
#include "facelock/storage.h"
#include "facelock/ipc_server.h"
#include "facelock/onnx_wrapper.h"
#include "facelock/version.h"

#include <filesystem>
#include <thread>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <syslog.h>
#include <systemd/sd-daemon.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

using namespace facelock;
namespace fs = std::filesystem;

// ============================================================
//  Daemon::Impl — shared state across all request handlers
// ============================================================
struct Daemon::Impl {
    std::unique_ptr<ONNXWrapper> onnx;
    std::mutex                   onnx_mtx;

    std::unique_ptr<CameraCapture> camera;

    bool load_onnx(const std::string& model_path) {
        std::lock_guard<std::mutex> lk(onnx_mtx);
        if (onnx) return true;
        try {
            onnx = std::make_unique<ONNXWrapper>(model_path);
            cv::Mat dummy(112, 112, CV_8UC3, cv::Scalar(128, 128, 128));
            onnx->warmup(dummy, 2);
            spdlog::info("ONNX session ready (model cached)");
            return true;
        } catch (const std::exception& e) {
            spdlog::error("ONNX load failed: {}", e.what());
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
//  Audit logger
// ============================================================
static void audit(const std::string& event,
                  const std::string& user,
                  bool               ok,
                  float              score     = -1.f,
                  float              threshold = -1.f,
                  const std::string& detail    = "")
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

    openlog("facelockd", LOG_PID, LOG_AUTHPRIV);
    syslog(ok ? LOG_INFO : LOG_WARNING, "%s", msg.c_str());
    closelog();
}

// ============================================================
//  Quality check (enrollment frames)
// ============================================================
static bool quality_ok(const cv::Mat& bgr)
{
    if (bgr.empty()) return false;

    cv::Mat gray, lap;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::Laplacian(gray, lap, CV_64F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(lap, mean, stddev);
    double sharpness = stddev.val[0] * stddev.val[0];
    if (sharpness < 30.0) {
        spdlog::debug("Quality reject: blurry (var={:.1f})", sharpness);
        return false;
    }

    cv::Scalar img_mean = cv::mean(gray);
    if (img_mean.val[0] < 20.0 || img_mean.val[0] > 240.0) {
        spdlog::debug("Quality reject: bad brightness (mean={:.1f})", img_mean.val[0]);
        return false;
    }
    return true;
}

// ============================================================
//  Cosine distance + top-K average
// ============================================================
static float cosine_distance(const std::vector<float>& a,
                              const std::vector<float>& b)
{
    float dot = 0.f, na = 0.f, nb = 0.f;
    const size_t D = a.size();
    for (size_t i = 0; i < D; ++i) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    return 1.0f - dot / (std::sqrt(na * nb) + 1e-12f);
}

static float top_k_avg(std::vector<float> dists, int k)
{
    const int take = std::min(k, static_cast<int>(dists.size()));
    std::partial_sort(dists.begin(), dists.begin() + take, dists.end());
    return std::accumulate(dists.begin(), dists.begin() + take, 0.f) / take;
}

// ============================================================
//  Daemon constructor / destructor
// ============================================================
Daemon::Daemon(const DaemonConfig& cfg)
    : pimpl_(std::make_unique<Impl>()), cfg_(cfg) {}

Daemon::~Daemon() = default;

// ============================================================
//  initialize — load ONNX + open camera + warmup
//  Called from run() before binding the socket or notifying systemd.
// ============================================================
bool Daemon::initialize()
{
    fs::create_directories(cfg_.data_dir);

    // --- ONNX ArcFace model ---
    if (!fs::exists(cfg_.onnx_model_path)) {
        spdlog::error("ArcFace model not found: {}", cfg_.onnx_model_path);
        spdlog::error("Run the installer or place the model at {}",
                      cfg_.onnx_model_path);
        return false;
    }
    if (!pimpl_->load_onnx(cfg_.onnx_model_path)) return false;

    // --- Camera + RetinaFace detector ---
    CaptureConfig cam_cfg;
    cam_cfg.camera_device          = cfg_.camera_device;
    cam_cfg.detector_model_path    = cfg_.detector_model_path;
    cam_cfg.detector_confidence    = cfg_.detector_confidence;
    cam_cfg.detector_nms           = cfg_.detector_nms;
    cam_cfg.liveness_enabled       = cfg_.liveness_enabled;
    cam_cfg.liveness_texture_min   = cfg_.liveness_texture_min;
    cam_cfg.liveness_blink_frames  = cfg_.liveness_blink_frames;
    cam_cfg.liveness_ear_threshold = cfg_.liveness_ear_threshold;

    pimpl_->camera = std::make_unique<CameraCapture>(cam_cfg);
    if (!pimpl_->camera->open()) {
        spdlog::error("Camera /dev/video{} failed to open", cfg_.camera_device);
        return false;
    }

    // Open camera on the main thread and keep it open permanently.
    // Worker threads (IPC thread pool) cannot open /dev/video* themselves
    // on cgroup v2 — opening once here on the main thread and sharing the
    // handle via cap_mtx_ is the correct approach.
    if (!pimpl_->camera->open_camera()) {
        spdlog::warn("Camera /dev/video{} unavailable at startup — "
                     "check CAMERA_DEVICE in /etc/facelock/facelock.conf",
                     cfg_.camera_device);
        // Non-fatal: daemon starts, auth falls back to password until camera is available
    } else {
        // Settle ISP — called on main thread before worker threads start
        pimpl_->camera->warmup(cfg_.camera_warmup_frames);
        spdlog::info("camera: /dev/video{} open and warmed", cfg_.camera_device);
    }

    spdlog::info("AstraLock {} daemon starting", FACELOCK_VERSION_STRING);
    spdlog::info("Model:     {}", cfg_.onnx_model_path);
    spdlog::info("Detector:  {}", cfg_.detector_model_path);
    spdlog::info("Threshold: {:.4f} (global fallback)", cfg_.onnx_threshold);
    spdlog::info("Camera:    /dev/video{}", cfg_.camera_device);
    spdlog::info("Liveness:  {}", cfg_.liveness_enabled ? "enabled" : "disabled");
    return true;
}

// ============================================================
//  handle_enroll
// ============================================================
json Daemon::handle_enroll(const std::string& user)
{
    fs::path userdir = fs::path(cfg_.data_dir) / user;
    fs::create_directories(userdir);

    std::vector<std::vector<float>> embeddings;
    int attempts      = 0;
    int quality_fails = 0;
    int liveness_fails = 0;
    auto start = std::chrono::steady_clock::now();

    spdlog::info("Enroll started for '{}'", user);

    // Liveness is disabled during enrollment — we need clean face samples,
    // not blink-verified ones. Liveness runs during auth only.
    // Temporarily set liveness_enabled=false on the camera for this session.
    pimpl_->camera->set_liveness(false);

    while ((int)embeddings.size() < cfg_.enroll_target && attempts < 80) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(60)) {
            spdlog::warn("Enroll timeout for '{}'", user);
            break;
        }

        cv::Mat face;
        CaptureResult result = pimpl_->camera->grab_aligned_face(face, 3000);

        if (result == CaptureResult::camera_error) {
            spdlog::error("Enroll: camera error for '{}'", user);
            break;
        }
        if (result == CaptureResult::liveness_fail) {
            ++liveness_fails;
            ++attempts;
            if (liveness_fails % 3 == 0)
                spdlog::info("Enroll '{}': {} liveness rejects — ensure you are "
                             "looking at the camera (not a photo/screen)", user, liveness_fails);
            continue;
        }
        if (result != CaptureResult::ok) {
            ++attempts;
            continue;
        }

        if (!quality_ok(face)) {
            ++quality_fails;
            ++attempts;
            if (quality_fails % 5 == 0)
                spdlog::info("Enroll '{}': {} quality rejects — ensure good "
                             "lighting and face the camera", user, quality_fails);
            continue;
        }

        // Save raw sample image for potential re-enroll later
        cv::imwrite(
            (userdir / (std::to_string(embeddings.size()) + ".png")).string(),
            face
        );

        auto emb = pimpl_->embed(face);
        if (!emb.empty())
            embeddings.push_back(std::move(emb));

        ++attempts;
    }

    // Re-enable liveness for auth requests
    pimpl_->camera->set_liveness(cfg_.liveness_enabled);

    const int got = static_cast<int>(embeddings.size());

    if (got < cfg_.enroll_min) {
        audit("enroll", user, false, -1.f, -1.f,
              fmt::format("only {} samples captured", got));
        return {{"v", 3}, {"ok", false}, {"err", "not_enough_faces"},
                 {"got", got}, {"need", cfg_.enroll_min},
                 {"hint", "Ensure camera can see your face clearly with "
                          "adequate lighting. Hold still during enrollment."}};
    }

    // ---- Per-user threshold calibration ----
    // Compute intra-class pairwise distances over the enrollment set,
    // then set threshold = mean + k * std_dev so the user's own natural
    // variation is captured, reducing false rejects for users with high
    // intra-class variance (glasses, facial hair, lighting changes).
    float calibrated_threshold = cfg_.onnx_threshold;
    {
        std::vector<float> pair_dists;
        pair_dists.reserve(got * (got - 1) / 2);
        for (int i = 0; i < got; ++i)
            for (int j = i + 1; j < got; ++j)
                pair_dists.push_back(cosine_distance(embeddings[i], embeddings[j]));

        if (!pair_dists.empty()) {
            float sum  = std::accumulate(pair_dists.begin(), pair_dists.end(), 0.f);
            float mean = sum / static_cast<float>(pair_dists.size());
            float sq_sum = 0.f;
            for (float d : pair_dists) sq_sum += (d - mean) * (d - mean);
            float std_dev = std::sqrt(sq_sum / static_cast<float>(pair_dists.size()));
            calibrated_threshold = mean + cfg_.enroll_threshold_k * std_dev;
            // Hard cap: never exceed the global threshold by more than 50%
            calibrated_threshold = std::min(calibrated_threshold,
                                            cfg_.onnx_threshold * 1.5f);
            // Hard floor: never go below 0.15 (too permissive)
            calibrated_threshold = std::max(calibrated_threshold, 0.15f);
            spdlog::info("Enroll '{}': calibrated threshold = {:.4f} "
                         "(mean={:.4f} std={:.4f})", user, calibrated_threshold, mean, std_dev);
        }
    }

    // ---- Persist ----
    EmbeddingStore store;
    store.embeddings = std::move(embeddings);
    store.threshold  = calibrated_threshold;

    if (!storage_save(cfg_.data_dir, user, store)) {
        audit("enroll", user, false, -1.f, -1.f, "write_failed");
        return {{"v", 3}, {"ok", false}, {"err", "write_failed"},
                 {"hint", "Check permissions on " + cfg_.data_dir}};
    }

    audit("enroll", user, true, -1.f, -1.f,
          fmt::format("samples={} quality_rejects={} liveness_rejects={} threshold={:.4f}",
                      got, quality_fails, liveness_fails, calibrated_threshold));

    return {{"v", 3}, {"ok", true}, {"samples", got},
             {"quality_rejects", quality_fails},
             {"liveness_rejects", liveness_fails},
             {"threshold", calibrated_threshold}};
}

// ============================================================
//  handle_auth
// ============================================================
json Daemon::handle_auth(const std::string& user)
{
    EmbeddingStore store;
    if (!storage_exists(cfg_.data_dir, user)) {
        audit("auth", user, false, -1.f, -1.f, "not_enrolled");
        return {{"v", 3}, {"ok", false}, {"err", "not_enrolled"},
                 {"match", false},
                 {"hint", "Run: facelock enroll " + user}};
    }

    if (!storage_load(cfg_.data_dir, user, store)) {
        audit("auth", user, false, -1.f, -1.f, "load_failed");
        return {{"v", 3}, {"ok", false}, {"err", "load_failed"}, {"match", false},
                 {"hint", "Embedding file is corrupt. Re-enroll: facelock enroll " + user}};
    }

    cv::Mat face;
    CaptureResult result = pimpl_->camera->grab_aligned_face(face);

    if (result == CaptureResult::liveness_fail) {
        audit("auth", user, false, -1.f, store.threshold, "liveness_fail");
        return {{"v", 3}, {"ok", false}, {"err", "liveness_fail"}, {"match", false},
                 {"hint", "Liveness check failed. Present your real face to the camera."}};
    }
    if (result != CaptureResult::ok) {
        audit("auth", user, false, -1.f, store.threshold, "no_face");
        return {{"v", 3}, {"ok", false}, {"err", "no_face"}, {"match", false},
                 {"hint", "Position your face in front of the camera and try again."}};
    }

    auto query = pimpl_->embed(face);
    if (query.empty()) {
        audit("auth", user, false, -1.f, store.threshold, "embed_failed");
        return {{"v", 3}, {"ok", false}, {"err", "embed_failed"}, {"match", false}};
    }

    // Top-3 cosine distance average
    std::vector<float> dists;
    dists.reserve(store.embeddings.size());
    for (const auto& e : store.embeddings)
        dists.push_back(cosine_distance(query, e));

    const float score = top_k_avg(dists, 3);
    const bool  match = score <= store.threshold;

    audit("auth", user, match, score, store.threshold);

    return {{"v", 3}, {"ok", true}, {"match", match},
             {"score", score}, {"threshold", store.threshold},
             {"err", nullptr}};
}

// ============================================================
//  handle_ping
// ============================================================
json Daemon::handle_ping()
{
    return {{"v", 3}, {"ok", true}, {"pong", true},
             {"version", FACELOCK_VERSION_STRING},
             {"camera_ready", pimpl_->camera && pimpl_->camera->is_ready()}};
}

// ============================================================
//  handle_list
// ============================================================
json Daemon::handle_list()
{
    auto users = storage_list(cfg_.data_dir);
    json arr = json::array();
    for (const auto& u : users) {
        EmbeddingStore s;
        if (storage_load(cfg_.data_dir, u, s)) {
            arr.push_back({{"user", u},
                           {"samples", s.embeddings.size()},
                           {"threshold", s.threshold}});
        } else {
            arr.push_back({{"user", u}, {"samples", 0}, {"threshold", 0.0f}});
        }
    }
    return {{"v", 3}, {"ok", true}, {"users", arr}};
}

// ============================================================
//  handle_delete
// ============================================================
json Daemon::handle_delete(const std::string& user)
{
    if (!storage_exists(cfg_.data_dir, user)) {
        return {{"v", 3}, {"ok", false}, {"err", "not_enrolled"},
                 {"hint", user + " is not enrolled"}};
    }
    if (!storage_delete(cfg_.data_dir, user)) {
        return {{"v", 3}, {"ok", false}, {"err", "delete_failed"}};
    }

    // Also remove sample images if present
    fs::path userdir = fs::path(cfg_.data_dir) / user;
    std::error_code ec;
    fs::remove_all(userdir, ec);

    audit("delete", user, true);
    return {{"v", 3}, {"ok", true}};
}

// ============================================================
//  handle_request — top-level dispatcher
// ============================================================
json Daemon::handle_request(const json& req)
{
    const std::string cmd  = req.value("cmd",  "");
    const std::string user = req.value("user", "");

    // Commands that don't require a user
    if (cmd == "ping")   return handle_ping();
    if (cmd == "list")   return handle_list();

    // All remaining commands require a user field
    if (user.empty())
        return {{"v", 3}, {"ok", false}, {"err", "no_user"},
                 {"hint", "Provide a 'user' field in the request"}};

    if (cmd == "enroll") return handle_enroll(user);
    if (cmd == "auth")   return handle_auth(user);
    if (cmd == "delete") return handle_delete(user);

    return {{"v", 3}, {"ok", false}, {"err", "unknown_cmd"},
             {"hint", "Valid commands: enroll, auth, ping, list, delete"}};
}

// ============================================================
//  run — entry point called from main()
// ============================================================
int Daemon::run()
{
    if (!initialize()) return 1;

    IPCServer server(cfg_.socket_path, cfg_.ipc_threads);
    if (!server.start([this](const json& r) { return handle_request(r); })) {
        spdlog::error("Failed to start IPC server on {}", cfg_.socket_path);
        return 1;
    }

    // Write ready file — install_facelock.sh ExecStartPost polls this
    {
        fs::path rp(cfg_.ready_file);
        fs::create_directories(rp.parent_path());
        FILE* f = fopen(cfg_.ready_file.c_str(), "w");
        if (f) { fputs("ready\n", f); fclose(f); }
    }

    // Notify systemd — daemon is fully ready.
    // display-manager.service is held until this fires (Before= in unit).
    sd_notify(0, "READY=1\nSTATUS=Listening for face auth requests");

    spdlog::info("Listening on {} ({} threads)", cfg_.socket_path, cfg_.ipc_threads);

    // Main thread just keeps the process alive.
    // IPCServer runs its thread pool internally.
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(60));
}
