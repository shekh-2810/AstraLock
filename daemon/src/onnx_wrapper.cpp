#include "facelock/onnx_wrapper.h"

#ifdef FACELOCK_ENABLE_ONNX

#include <onnxruntime_cxx_api.h>
#include <stdexcept>
#include <vector>
#include <cmath>
#include <iostream>

using namespace facelock;

// ===================== REAL IMPLEMENTATION =====================

struct ONNXWrapper::Impl {
    Ort::Env env;
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> session;
    std::string model_path;
    std::string input_name;
    std::vector<std::string> output_names;
    std::pair<int,int> input_size = {112,112};

    Impl(const std::string &model)
        : env(ORT_LOGGING_LEVEL_WARNING, "facelock"), model_path(model)
    {
        opts.SetIntraOpNumThreads(1);

        session = std::make_unique<Ort::Session>(env, model_path.c_str(), opts);

        // Input name
        try {
            auto names = session->GetInputNames();
            if (!names.empty()) input_name = names[0];
        } catch (...) {}

        // Output names
        try {
            output_names = session->GetOutputNames();
        } catch (...) {}

        // Input shape
        try {
            auto info = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (shape.size() >= 4) {
                int h = shape[2] > 0 ? shape[2] : 112;
                int w = shape[3] > 0 ? shape[3] : 112;
                input_size = {w, h};
            }
        } catch (...) {}
    }
};

// ---------- helpers ----------
static void hwc_to_chw(const cv::Mat &src, std::vector<float> &out) {
    int H = src.rows, W = src.cols;
    out.resize(3 * H * W);
    for (int c = 0; c < 3; ++c)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                out[c * H * W + y * W + x] = src.at<cv::Vec3f>(y,x)[c];
}

// ---------- public API ----------
ONNXWrapper::ONNXWrapper(const std::string &model_path,
                         const std::vector<std::string>&)
{
    pimpl_ = new Impl(model_path);
    input_size_ = pimpl_->input_size;
}

ONNXWrapper::~ONNXWrapper() {
    delete pimpl_;
}

std::vector<float> ONNXWrapper::embed(const cv::Mat &bgr) {
    auto [W,H] = pimpl_->input_size;

    cv::Mat rgb, resized;
    cv::cvtColor(bgr, rgb, bgr.channels()==1 ? cv::COLOR_GRAY2RGB : cv::COLOR_BGR2RGB);
    cv::resize(rgb, resized, {W,H});
    resized.convertTo(resized, CV_32FC3, 1.0/255.0);

    std::vector<float> input;
    hwc_to_chw(resized, input);

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(
        OrtDeviceAllocator, OrtMemTypeCPU);

    std::vector<int64_t> shape = {1,3,H,W};
    Ort::Value tensor = Ort::Value::CreateTensor<float>(
        mem, input.data(), input.size(), shape.data(), shape.size());

    const char* in_name = pimpl_->input_name.c_str();
    std::vector<const char*> out_names;
    for (auto &s : pimpl_->output_names) out_names.push_back(s.c_str());

    auto outputs = pimpl_->session->Run(
        Ort::RunOptions{nullptr},
        &in_name, &tensor, 1,
        out_names.data(), out_names.size());

    float* ptr = outputs[0].GetTensorMutableData<float>();
    size_t n = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();

    std::vector<float> emb(ptr, ptr+n);

    // L2 normalize
    float norm = 0.f;
    for (float v : emb) norm += v*v;
    norm = std::sqrt(norm);
    if (norm > 1e-6f)
        for (auto &v : emb) v /= norm;

    return emb;
}

std::vector<float> ONNXWrapper::run_raw(const cv::Mat &img) {
    return embed(img);
}

void ONNXWrapper::warmup(const cv::Mat &sample, int runs) {
    for (int i=0;i<runs;i++) embed(sample);
}

#else  // ===================== STUB IMPLEMENTATION =====================

using namespace facelock;

ONNXWrapper::ONNXWrapper(const std::string&,
                         const std::vector<std::string>&)
{
    throw std::runtime_error("ONNX support disabled at build time");
}

ONNXWrapper::~ONNXWrapper() = default;

std::vector<float> ONNXWrapper::embed(const cv::Mat&) {
    throw std::runtime_error("ONNX support disabled");
}

std::vector<float> ONNXWrapper::run_raw(const cv::Mat&) {
    throw std::runtime_error("ONNX support disabled");
}

void ONNXWrapper::warmup(const cv::Mat&, int) {}

#endif
