#include "facelock/onnx_wrapper.h"

#ifdef FACELOCK_ENABLE_ONNX

// Try the installed package path first, fall back to manual install path
#if __has_include(<onnxruntime/onnxruntime_cxx_api.h>)
#  include <onnxruntime/onnxruntime_cxx_api.h>
#elif __has_include(<onnxruntime_cxx_api.h>)
#  include <onnxruntime_cxx_api.h>
#else
#  error "Cannot find onnxruntime_cxx_api.h — check your ONNX Runtime installation"
#endif

#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <vector>
#include <cmath>

using namespace facelock;

// ============================================================
//  Impl — owns the ONNX Runtime session
// ============================================================
struct ONNXWrapper::Impl {
    Ort::Env                       env;
    Ort::SessionOptions            opts;
    std::unique_ptr<Ort::Session>  session;
    std::string                    input_name;
    std::vector<std::string>       output_names;
    std::pair<int,int>             input_size = {112, 112};

    explicit Impl(const std::string& model_path)
        : env(ORT_LOGGING_LEVEL_WARNING, "facelockd")
    {
        opts.SetIntraOpNumThreads(1);

        session = std::make_unique<Ort::Session>(
            env, model_path.c_str(), opts);

        // Cache input/output names via GetInputNameAllocated/GetOutputNameAllocated —
        // the Session::GetInputNames()/GetOutputNames() convenience wrappers used
        // here previously don't exist on ONNX Runtime 1.17.x (the version this
        // project's installer fetches); GetXNameAllocated() is available across
        // both older and newer releases.
        Ort::AllocatorWithDefaultOptions allocator;

        try {
            if (session->GetInputCount() > 0) {
                auto name_ptr = session->GetInputNameAllocated(0, allocator);
                input_name = name_ptr.get();
            }
        } catch (...) {}

        try {
            const size_t n_outputs = session->GetOutputCount();
            output_names.reserve(n_outputs);
            for (size_t i = 0; i < n_outputs; ++i) {
                auto name_ptr = session->GetOutputNameAllocated(i, allocator);
                output_names.emplace_back(name_ptr.get());
            }
        } catch (...) {}

        // Cache input spatial size from model metadata
        try {
            auto info  = session->GetInputTypeInfo(0)
                                 .GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (shape.size() >= 4) {
                int h = shape[2] > 0 ? static_cast<int>(shape[2]) : 112;
                int w = shape[3] > 0 ? static_cast<int>(shape[3]) : 112;
                input_size = {w, h};
            }
        } catch (...) {}
    }
};

// ============================================================
//  Internal helper — HWC BGR float → CHW float (ONNX input format)
// ============================================================
static void hwc_to_chw(const cv::Mat& src, std::vector<float>& out)
{
    const int H = src.rows, W = src.cols;
    out.resize(static_cast<size_t>(3 * H * W));
    for (int c = 0; c < 3; ++c)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                out[static_cast<size_t>(c * H * W + y * W + x)] =
                    src.at<cv::Vec3f>(y, x)[c];
}

// ============================================================
//  Public API
// ============================================================
ONNXWrapper::ONNXWrapper(const std::string& model_path,
                         const std::vector<std::string>& /*providers*/)
    : pimpl_(std::make_unique<Impl>(model_path))
{
    input_size_ = pimpl_->input_size;
}

// unique_ptr<Impl> destructor handles cleanup — no explicit delete needed
ONNXWrapper::~ONNXWrapper() = default;

std::vector<float> ONNXWrapper::embed(const cv::Mat& bgr)
{
    const auto [W, H] = pimpl_->input_size;

    // BGR → RGB, resize, normalise to [0,1]
    cv::Mat rgb, resized;
    cv::cvtColor(bgr,
                 rgb,
                 bgr.channels() == 1 ? cv::COLOR_GRAY2RGB
                                     : cv::COLOR_BGR2RGB);
    cv::resize(rgb, resized, {W, H});
    resized.convertTo(resized, CV_32FC3, 1.0 / 255.0);

    std::vector<float> input;
    hwc_to_chw(resized, input);

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(
        OrtDeviceAllocator, OrtMemTypeCPU);

    std::vector<int64_t>  shape  = {1, 3, H, W};
    Ort::Value            tensor = Ort::Value::CreateTensor<float>(
        mem, input.data(), input.size(),
        shape.data(), shape.size());

    const char* in_name = pimpl_->input_name.c_str();
    std::vector<const char*> out_names;
    out_names.reserve(pimpl_->output_names.size());
    for (const auto& s : pimpl_->output_names)
        out_names.push_back(s.c_str());

    auto outputs = pimpl_->session->Run(
        Ort::RunOptions{nullptr},
        &in_name, &tensor, 1,
        out_names.data(), out_names.size());

    const float* ptr = outputs[0].GetTensorMutableData<float>();
    const size_t n   = outputs[0].GetTensorTypeAndShapeInfo()
                                  .GetElementCount();

    std::vector<float> emb(ptr, ptr + n);

    // L2 normalise
    float norm = 0.f;
    for (float v : emb) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 1e-6f)
        for (auto& v : emb) v /= norm;

    return emb;
}

std::vector<float> ONNXWrapper::run_raw(const cv::Mat& img)
{
    return embed(img);
}

void ONNXWrapper::warmup(const cv::Mat& sample, int runs)
{
    for (int i = 0; i < runs; ++i) embed(sample);
}

#else  // ── STUB — compiled when FACELOCK_ENABLE_ONNX is not defined ──────

using namespace facelock;

ONNXWrapper::ONNXWrapper(const std::string&,
                         const std::vector<std::string>&)
{
    throw std::runtime_error(
        "ONNX support was disabled at build time. "
        "Ensure FACELOCK_ENABLE_ONNX=1 is set in CMake.");
}

ONNXWrapper::~ONNXWrapper() = default;

std::vector<float> ONNXWrapper::embed(const cv::Mat&)
{
    throw std::runtime_error("ONNX support disabled");
}

std::vector<float> ONNXWrapper::run_raw(const cv::Mat&)
{
    throw std::runtime_error("ONNX support disabled");
}

void ONNXWrapper::warmup(const cv::Mat&, int) {}

#endif