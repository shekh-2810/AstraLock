#include "facelock/camera.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/videoio.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <cmath>
#include <vector>
#include <numeric>

namespace facelock {

static const cv::Point2f ARCFACE_DST[5] = {
    {38.2946f, 51.6963f},
    {73.5318f, 51.5014f},
    {56.0252f, 71.7366f},
    {41.5493f, 92.3655f},
    {70.7299f, 92.2041f},
};

static cv::Mat align_to_arcface(const cv::Mat& frame,
                                const cv::Mat& det, int idx)
{
    cv::Point2f src[5];
    for (int i = 0; i < 5; ++i)
        src[i] = {det.at<float>(idx, 4+i*2), det.at<float>(idx, 4+i*2+1)};

    cv::Mat transform = cv::estimateAffinePartial2D(
        std::vector<cv::Point2f>(src, src+5),
        std::vector<cv::Point2f>(ARCFACE_DST, ARCFACE_DST+5));
    if (transform.empty()) return {};

    cv::Mat aligned;
    cv::warpAffine(frame, aligned, transform, {112,112},
                   cv::INTER_LINEAR, cv::BORDER_REFLECT);
    return aligned;
}

// ============================================================
//  Impl
//
//  Design (v3 simplified):
//    - Detector loaded ONCE at startup (weights only, no hardware).
//    - VideoCapture opened per-request, released immediately after.
//    - Camera LED is off when idle.
//    - Liveness disabled — reserved for future beta release.
//    - Single mutex protects the detector (shared ONNX-adjacent resource).
// ============================================================
struct CameraCapture::Impl {
    CaptureConfig               cfg;
    cv::Ptr<cv::FaceDetectorYN> detector;
    mutable std::mutex          mtx;
    bool                        detector_ready = false;

    explicit Impl(const CaptureConfig& c) : cfg(c) {}

    bool load_detector() {
        try {
            detector = cv::FaceDetectorYN::create(
                cfg.detector_model_path, "", {640, 480},
                cfg.detector_confidence, cfg.detector_nms, 5000);
        } catch (const cv::Exception& e) {
            spdlog::error("camera: detector load failed: {}", e.what());
            return false;
        }
        if (detector.empty()) {
            spdlog::error("camera: detector null after create()");
            return false;
        }
        detector_ready = true;
        spdlog::info("camera: RetinaFace detector loaded");
        return true;
    }

    // Open VideoCapture using integer index with retry.
    // Returns an open cap or logs error and returns closed cap.
    cv::VideoCapture open_cap() {
        cv::VideoCapture cap;
        for (int i = 0; i < 6; ++i) {
            cap.open(cfg.camera_device, cv::CAP_V4L2);
            if (cap.isOpened()) {
                cap.set(cv::CAP_PROP_FRAME_WIDTH,  640);
                cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
                cap.set(cv::CAP_PROP_BUFFERSIZE,   2);
                return cap;
            }
            spdlog::warn("camera: open attempt {}/6 on /dev/video{} failed",
                         i+1, cfg.camera_device);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        spdlog::error("camera: cannot open /dev/video{} after 6 attempts",
                      cfg.camera_device);
        return cap;  // closed
    }

    // Discard N frames to let ISP auto-exposure settle
    void settle(cv::VideoCapture& cap, int n = 4) {
        cv::Mat dummy;
        for (int i = 0; i < n; ++i) cap.read(dummy);
    }
};

// ============================================================
//  Public interface
// ============================================================

CameraCapture::CameraCapture(const CaptureConfig& cfg)
    : pimpl_(std::make_unique<Impl>(cfg)) {}

CameraCapture::~CameraCapture() = default;

// Load detector model — called once at daemon startup
bool CameraCapture::open() {
    if (pimpl_->detector_ready) return true;
    return pimpl_->load_detector();
}

bool CameraCapture::is_ready() const {
    return pimpl_->detector_ready;
}

// open_camera / close_camera / camera_is_open — stubs kept for API compat
// Camera is now opened per-request inside grab_aligned_face
bool CameraCapture::open_camera()   { return pimpl_->detector_ready; }
void CameraCapture::close_camera()  {}
bool CameraCapture::camera_is_open() const { return false; }
void CameraCapture::set_liveness(bool) {}  // liveness is beta — no-op for now

// warmup — quick open/settle/close to pre-init kernel V4L2 driver
// Camera LED flickers briefly at startup then goes off
void CameraCapture::warmup(int count) {
    if (!pimpl_->detector_ready) return;
    auto cap = pimpl_->open_cap();
    if (!cap.isOpened()) {
        spdlog::warn("camera: warmup skipped — camera unavailable");
        return;
    }
    pimpl_->settle(cap, count);
    cap.release();
    spdlog::info("camera: warmup complete ({} frames), LED off", count);
}

void CameraCapture::close() {}

// ── grab_aligned_face ─────────────────────────────────────────────────────────
//
//  Opens camera → settles ISP → grabs frames until face found → aligns →
//  closes camera (LED off).  All within timeout_ms.
//
//  Liveness: disabled (future beta). Returns ok on any aligned face.
//
CaptureResult CameraCapture::grab_aligned_face(cv::Mat& face_bgr, int timeout_ms)
{
    std::lock_guard<std::mutex> lk(pimpl_->mtx);

    if (!pimpl_->detector_ready) {
        spdlog::warn("camera: grab called before detector loaded");
        return CaptureResult::camera_error;
    }

    if (timeout_ms <= 0) timeout_ms = 5000;

    // Open camera for this request
    auto cap = pimpl_->open_cap();
    if (!cap.isOpened()) return CaptureResult::camera_error;

    // Settle ISP — 4 frames (~130ms at 30fps)
    pimpl_->settle(cap, 4);

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);

    cv::Mat frame, dets;
    int best_idx = -1;

    while (std::chrono::steady_clock::now() < deadline) {
        if (!cap.read(frame) || frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }
        pimpl_->detector->detect(frame, dets);
        if (dets.rows > 0) {
            best_idx = 0;
            float bc = dets.at<float>(0, 14);
            for (int i = 1; i < dets.rows; ++i) {
                float c = dets.at<float>(i, 14);
                if (c > bc) { bc = c; best_idx = i; }
            }
            break;
        }
    }

    cap.release();  // LED off — always released before return

    if (best_idx < 0) {
        spdlog::debug("camera: no face detected within {}ms", timeout_ms);
        return CaptureResult::timeout;
    }

    cv::Mat aligned = align_to_arcface(frame, dets, best_idx);
    if (aligned.empty()) {
        spdlog::warn("camera: alignment failed");
        return CaptureResult::no_face;
    }

    face_bgr = std::move(aligned);
    return CaptureResult::ok;
}

} // namespace facelock