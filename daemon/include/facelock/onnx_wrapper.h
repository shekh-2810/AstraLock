#pragma once

#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <opencv2/core.hpp>

namespace facelock {

// ============================================================
//  ONNXWrapper — thin RAII wrapper around an ONNX Runtime session.
//
//  One instance = one loaded model.
//  Daemon::Impl holds a single shared instance (cached) behind a
//  mutex so concurrent requests share the same session.
//
//  Interface is unchanged from v2.1 — only the pimpl pointer is
//  upgraded from raw to unique_ptr so the destructor is implicit.
// ============================================================
class ONNXWrapper {
public:
    // model_path: absolute path to the .onnx file
    // providers:  optional execution provider list
    //             (default: CPUExecutionProvider)
    explicit ONNXWrapper(const std::string& model_path,
                         const std::vector<std::string>& providers = {});
    ~ONNXWrapper();

    // Non-copyable — sessions are not cheaply copyable
    ONNXWrapper(const ONNXWrapper&)            = delete;
    ONNXWrapper& operator=(const ONNXWrapper&) = delete;

    // Compute a normalised L2 embedding for a 112×112 BGR face crop.
    // Returns an empty vector on error.
    std::vector<float> embed(const cv::Mat& bgr_crop);

    // Run the model and return the raw first output tensor.
    // Useful for detector models where no L2 normalisation is needed.
    std::vector<float> run_raw(const cv::Mat& bgr_input);

    // Warmup: run `runs` dummy inferences to load CUDA/model weights into
    // cache.  Call once after construction before serving real requests.
    void warmup(const cv::Mat& sample, int runs = 2);

    // Returns the (width, height) the model expects as input.
    std::pair<int,int> input_size() const { return input_size_; }

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    std::pair<int,int>    input_size_ = {112, 112};
};

} // namespace facelock