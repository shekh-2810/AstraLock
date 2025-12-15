#pragma once
#include <string>
#include <vector>
#include <optional>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace facelock {

struct LandmarkResult {
    // normalized coordinates [0..1] relative to input image width/height
    std::vector<cv::Point2f> landmarks;
};

class FaceAligner {
public:
    FaceAligner() = default;
    ~FaceAligner();

    // load an ONNX landmark model (expects NCHW float32 input)
    // model_path: onnx model file path
    // input_size: model input (width,height) e.g. (112,112) or (160,160)
    bool load_onnx(const std::string &model_path, int input_width=112, int input_height=112);

    // run landmark detection on a BGR image (any size). Returns normalized landmarks in image coords.
    std::optional<LandmarkResult> detect_landmarks(const cv::Mat &bgr);

    // produce a 200x200 aligned gray crop suitable for LBPH from landmarks:
    // - uses chosen eye/nose landmarks heuristics; returns empty optional if alignment failed.
    std::optional<cv::Mat> align_crop_for_lbph(const cv::Mat &bgr);

    // quick pose hints from landmarks: "center"/"left"/"right"/"up"/"down"/"tilt_left"/"tilt_right"
    std::string pose_hint(const LandmarkResult &lr, const cv::Size &img_size) const;

private:
    struct Impl;
    Impl *pimpl_ = nullptr;
};

} // namespace facelock
