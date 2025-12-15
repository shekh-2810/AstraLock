// include/facelock/onnx_wrapper.h
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace facelock {

class ONNXWrapper {
public:
    // model_path: path to ONNX file
    // providers: optional provider list (e.g., {"CPUExecutionProvider"})
    ONNXWrapper(const std::string& model_path,
                const std::vector<std::string>& providers = {});
    ~ONNXWrapper();

    // compute normalized embedding for a BGR image crop
    // typical use: face recognition embedding nets
    std::vector<float> embed(const cv::Mat& bgr_crop);

    // run model and return raw float output (first output) — useful for landmark models
    std::vector<float> run_raw(const cv::Mat& bgr_input);

    // warmup: run a couple dummy inferences
    void warmup(const cv::Mat& sample, int runs = 2);

    // get input size (width,height)
    std::pair<int,int> input_size() const { return input_size_; }

private:
    struct Impl;
    Impl* pimpl_;

    // internal cached model input size
    std::pair<int,int> input_size_ = {112,112};
};

} // namespace facelock
