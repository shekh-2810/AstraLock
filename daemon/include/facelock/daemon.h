#pragma once

#include <string>
#include <memory>
#include <nlohmann/json.hpp>

namespace facelock {

using json = nlohmann::json;

// ============================================================
//  DaemonConfig — all runtime tunables.
//
//  Loaded from /etc/facelock/facelock.conf (KEY=VALUE).
//  Any key not present in the file keeps its compiled default.
//  Validated in Daemon::initialize() — invalid values are
//  logged and replaced with the default, never silently accepted.
// ============================================================
struct DaemonConfig {
    // --- IPC ---
    std::string socket_path     = "/run/facelock/facelock.sock";

    // --- Storage ---
    std::string data_dir        = "/var/lib/facelock/";

    // --- Models ---
    std::string onnx_model_path =
        "/usr/share/facelock/models/w600k_mbf.onnx";
    std::string detector_model_path =
        "/usr/share/facelock/models/retinaface.onnx";
    float detector_confidence    = 0.6f;
    float detector_nms           = 0.3f;

    // --- Scoring ---
    // Global fallback threshold (used when no per-user threshold exists).
    // Per-user threshold is stored in the embedding file and takes priority.
    float onnx_threshold        = 0.30f;

    // --- Camera ---
    int   camera_device         = 0;      // /dev/videoN

    // --- Enrollment ---
    int   enroll_target         = 20;     // desired sample count
    int   enroll_min            = 10;     // minimum accepted
    // k-sigma multiplier for per-user threshold calibration at enroll time:
    //   threshold = mean_dist + ENROLL_THRESHOLD_K * std_dist
    float enroll_threshold_k    = 1.5f;

    // --- Liveness ---
    // NOT YET ENFORCED — see camera.h CaptureConfig comment. Defaults to
    // false so an unmodified install doesn't log a misleading "enabled".
    bool  liveness_enabled      = false;
    float liveness_texture_min  = 0.15f;
    int   liveness_blink_frames = 12;
    float liveness_ear_threshold = 0.12f;

    // --- Thread pool ---
    int   ipc_threads           = 4;      // concurrent request handlers

    // --- Startup ---
    int   camera_warmup_frames  = 5;      // frames grabbed before READY=1

    // --- Ready file (written when daemon is fully initialised) ---
    // systemd ExecStartPost waits on this before starting display-manager.
    std::string ready_file      = "/run/facelock/ready";
};

// ============================================================
//  Daemon
// ============================================================
class Daemon {
public:
    explicit Daemon(const DaemonConfig& cfg);
    ~Daemon();

    // Non-copyable
    Daemon(const Daemon&)            = delete;
    Daemon& operator=(const Daemon&) = delete;

    // Load models, open camera, warmup, notify systemd, bind socket.
    // Returns non-zero on fatal error.
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    DaemonConfig cfg_;

    bool initialize();
    json handle_request(const json& req);
    // force=false (cmd "enroll"): refuses to run if the user already has
    // saved embeddings, so a stray/reflexive re-run never silently
    // overwrites data (e.g. data kept across an uninstall/reinstall).
    // force=true  (cmd "reenroll"): explicit, intentional overwrite.
    json handle_enroll(const std::string& user, bool force);
    json handle_auth(const std::string& user);
    json handle_ping();
    json handle_list();
    json handle_delete(const std::string& user);
};

} // namespace facelock
