#pragma once

#include <optional>
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

// If the buildsystem already defined FACELD_HAVE_OPENCV_FACE, honor it.
// Otherwise detect OpenCV face header availability and define it here.
#ifndef FACELD_HAVE_OPENCV_FACE
#  if __has_include(<opencv2/face.hpp>)
#    include <opencv2/face.hpp>
#    define FACELD_HAVE_OPENCV_FACE 1
#  else
#    define FACELD_HAVE_OPENCV_FACE 0
#  endif
#endif

namespace facelock {

struct LBPHResult {
    bool match;
    float score; // lower = better
    std::string best_sample; // filename or index
};

class LBPHWrapper {
public:
    LBPHWrapper() = default;
    ~LBPHWrapper();

    bool load(const std::string &user, const std::string &model_dir = "data/models");
    std::optional<LBPHResult> predict(const cv::Mat &gray_crop, float threshold = 0.30f);
    std::string backend() const { return backend_; }

private:
    std::string backend_ = "none";

#if FACELD_HAVE_OPENCV_FACE
    cv::Ptr<cv::face::LBPHFaceRecognizer> cv_recog_;
#else
    void *cv_recog_ = nullptr;
#endif

    std::vector<std::vector<float>> embeddings_;
    std::vector<std::string> emb_names_;
    size_t emb_dim_ = 0;
};

} // namespace facelock
