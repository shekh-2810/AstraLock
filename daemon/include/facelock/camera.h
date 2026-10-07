#pragma once

#include <string>
#include <memory>
#include <opencv2/core.hpp>

namespace facelock {

struct CaptureConfig {
    std::string detector_model_path =
        "/usr/share/facelock/models/retinaface.onnx";
    int   camera_device          = 0;    // /dev/videoN index (used when camera_path is empty)
    // Persistent device path, e.g. /dev/v4l/by-id/usb-...-video-index0.
    // When set, takes priority over camera_device and is re-resolved on
    // every open, so USB re-enumeration or a virtual camera (OBS) shifting
    // /dev/video* numbers does not break auth.
    std::string camera_path;
    float detector_confidence    = 0.6f;
    float detector_nms           = 0.3f;
    // NOT YET ENFORCED — reserved for a future anti-spoofing implementation.
    // grab_aligned_face() does not currently check any of these; a match
    // is accepted on any detected face regardless of these values.
    bool  liveness_enabled       = false;
    float liveness_texture_min   = 0.15f;
    int   liveness_blink_frames  = 10;    // frames to observe for blink
    float liveness_ear_threshold = 0.12f; // loosened — 5-pt landmark proxy
};

enum class CaptureResult {
    ok,
    no_face,
    liveness_fail,
    camera_error,
    timeout,
};

class CameraCapture {
public:
    explicit CameraCapture(const CaptureConfig& cfg = {});
    ~CameraCapture();

    CameraCapture(const CameraCapture&)            = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    // Load the detector model. Must be called once at startup.
    bool open();

    bool is_ready() const;

    // Human-readable description of the configured camera for logs, e.g.
    // "/dev/video2 (via /dev/v4l/by-id/...)" or "/dev/video0".
    std::string device_description() const;

    // ── Session management ────────────────────────────────────────────────────
    // For enrollment: call open_camera() once, grab_aligned_face() N times,
    // then close_camera().  Avoids the per-sample open/settle/close overhead.
    // For auth: grab_aligned_face() handles open/close internally.
    bool open_camera();   // open VideoCapture (turns LED on)
    void close_camera();  // release VideoCapture (turns LED off)
    bool camera_is_open() const;

    // Grab one aligned face crop.
    // If camera is already open (enrollment session), uses it directly.
    // If not open (auth), opens internally, captures, closes.
    CaptureResult grab_aligned_face(cv::Mat& face_bgr,
                                    int timeout_ms = 5000);

    // Quick open→grab→close at daemon startup to pre-init the kernel driver.
    void warmup(int count = 5);

    void close();  // unload detector

    // Toggle liveness check at runtime (disabled during enrollment)
    void set_liveness(bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace facelock
