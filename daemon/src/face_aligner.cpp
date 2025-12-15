#include "facelock/face_aligner.h"
#include "facelock/onnx_wrapper.h" // your existing ONNX wrapper
#include <cmath>

using namespace facelock;

struct FaceAligner::Impl {
    std::unique_ptr<ONNXWrapper> sess;
    int input_w = 112;
    int input_h = 112;
    // how many landmarks expected (inferred from model output)
    int n_landmarks = 0;
};

FaceAligner::~FaceAligner(){ delete pimpl_; }

bool FaceAligner::load_onnx(const std::string &model_path, int input_width, int input_height) {
    if(!pimpl_) pimpl_ = new Impl();
    pimpl_->input_w = input_width;
    pimpl_->input_h = input_height;
    try {
        pimpl_->sess = std::make_unique<ONNXWrapper>(model_path, std::vector<std::string>{});
    } catch(std::exception &e) {
        return false;
    }
    // attempt to infer output size by running a dummy
    cv::Mat dummy = cv::Mat::zeros(pimpl_->input_h, pimpl_->input_w, CV_8UC3);
    try {
        auto out = pimpl_->sess->embed(dummy); // reuse embed path if your ONNXWrapper provides generic run; otherwise call a dedicated run in ONNXWrapper
        // expecting out vector length = n_landmarks*2
        pimpl_->n_landmarks = static_cast<int>(out.size()/1)/2; // if out is flattened landmarks
        if(pimpl_->n_landmarks <= 0) pimpl_->n_landmarks = 0; // keep 0 if unknown
    } catch(...) {
        // ignore — not critical
    }
    return true;
}

std::optional<LandmarkResult> FaceAligner::detect_landmarks(const cv::Mat &bgr) {
    if(!pimpl_ || !pimpl_->sess) return std::nullopt;
    // Preprocess: BGR->RGB, resize to model input, normalize [0,1]
    cv::Mat rgb;
    if(bgr.channels() == 1) cv::cvtColor(bgr, rgb, cv::COLOR_GRAY2BGR);
    else cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    cv::Mat resized;
    cv::resize(rgb, resized, cv::Size(pimpl_->input_w, pimpl_->input_h));
    // ONNXWrapper::embed currently returns float vector outputs; we expect landmark vector here.
    std::vector<float> out;
    try {
        out = pimpl_->sess->embed(resized); // NOTE: assumes ONNXWrapper::embed returns model output
    } catch(...) {
        return std::nullopt;
    }
    if(out.empty()) return std::nullopt;
    // Interpret output: assume flat [x0,y0,x1,y1,...] normalized [0..1]
    size_t M = out.size();
    if(M % 2 != 0) return std::nullopt;
    size_t L = M/2;
    LandmarkResult lr;
    lr.landmarks.reserve(L);
    for(size_t i=0;i<L;i++){
        float nx = out[2*i];     // normalized in [0..1]
        float ny = out[2*i+1];
        // Map back to original image coordinates (we resized the input; so convert)
        // But since we resized, we can map relative positions to resized image, then scale to source bgr
        int sx = static_cast<int>(nx * pimpl_->input_w + 0.5f);
        int sy = static_cast<int>(ny * pimpl_->input_h + 0.5f);
        // scale to original bgr size
        float fx = (float)sx / (float)pimpl_->input_w * (float)bgr.cols;
        float fy = (float)sy / (float)pimpl_->input_h * (float)bgr.rows;
        lr.landmarks.push_back(cv::Point2f(fx, fy));
    }
    return lr;
}

std::optional<cv::Mat> FaceAligner::align_crop_for_lbph(const cv::Mat &bgr) {
    auto opt = detect_landmarks(bgr);
    if(!opt) return std::nullopt;
    LandmarkResult lr = *opt;
    // Heuristic: use eye centers and nose to compute similarity transform
    // pick left eye index and right eye index, and nose tip; these indices depend on model!
    // Provide flexible approach: if many landmarks, use typical indices; else fallback to bounding box center.
    cv::Point2f left_eye, right_eye, nose;
    if(lr.landmarks.size() >= 468) {
        // MediaPipe-like indexing: left-eye ~ 33/133, right-eye ~ 362/263, nose tip ~ 1
        left_eye = lr.landmarks[33];
        right_eye = lr.landmarks[263];
        nose = lr.landmarks[1];
    } else if(lr.landmarks.size() >= 68) {
        // 68-point: left eye average (36..41), right eye (42..47), nose tip ~ 30
        cv::Point2f le(0,0), re(0,0);
        for(int i=36;i<=41;i++) le += lr.landmarks[i];
        for(int i=42;i<=47;i++) re += lr.landmarks[i];
        left_eye = le * (1.0f/6.0f);
        right_eye = re * (1.0f/6.0f);
        nose = lr.landmarks[30];
    } else {
        // fallback: center crop around mean of landmarks
        cv::Point2f mean(0,0);
        for(auto &p: lr.landmarks) mean += p;
        mean *= (1.0f / lr.landmarks.size());
        int size = std::min(bgr.cols, bgr.rows) / 2;
        int x0 = std::max(0, int(mean.x - size/2));
        int y0 = std::max(0, int(mean.y - size/2));
        cv::Rect r(x0, y0, std::min(size, bgr.cols - x0), std::min(size, bgr.rows - y0));
        cv::Mat crop = bgr(r).clone();
        cv::Mat gray;
        if(crop.channels()==3) cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY); else gray = crop;
        cv::Mat resized; cv::resize(gray, resized, cv::Size(200,200));
        return resized;
    }

    // Compute similarity transform that maps eye midline to horizontal center
    cv::Point2f eyes_center = (left_eye + right_eye) * 0.5f;
    float dx = right_eye.x - left_eye.x;
    float dy = right_eye.y - left_eye.y;
    float angle = atan2f(dy, dx) * 180.0f / CV_PI;
    float desired_left_eye_x = 0.35f; // desired left eye pos in normalized output
    float desired_dist = (1.0f - 2.0f*desired_left_eye_x) * 200.0f;
    float dist = sqrtf(dx*dx + dy*dy);
    float scale = desired_dist / (dist + 1e-6f);

    // get rotation matrix
    cv::Mat M = cv::getRotationMatrix2D(eyes_center, angle, scale);
    // translate: put eyes_center to desired location
    float ex = 200.0f * 0.5f;
    float ey = 200.0f * 0.4f;
    M.at<double>(0,2) += ex - eyes_center.x;
    M.at<double>(1,2) += ey - eyes_center.y;

    cv::Mat warped;
    cv::warpAffine(bgr, warped, M, cv::Size(200,200), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    cv::Mat gray;
    if(warped.channels()==3) cv::cvtColor(warped, gray, cv::COLOR_BGR2GRAY); else gray = warped;
    cv::equalizeHist(gray, gray);
    return gray;
}

std::string FaceAligner::pose_hint(const LandmarkResult &lr, const cv::Size &img_size) const {
    if(lr.landmarks.empty()) return "no_face";
    cv::Point2f center(0,0);
    for(auto &p: lr.landmarks) center += p;
    center *= (1.0f / lr.landmarks.size());
    float cx = img_size.width * 0.5f;
    float cy = img_size.height * 0.5f;
    float dx = center.x - cx;
    float dy = center.y - cy;
    if(std::abs(dx) < img_size.width * 0.12f && std::abs(dy) < img_size.height * 0.08f) return "center";
    if(dx < -img_size.width * 0.12f) return "turn_right"; // face center left -> user turned right
    if(dx > img_size.width * 0.12f) return "turn_left";
    if(dy < -img_size.height * 0.10f) return "look_up";
    if(dy > img_size.height * 0.10f) return "look_down";
    // tilt detection: estimate eye vertical difference
    if(lr.landmarks.size() >= 68) {
        float left_eye_y=0, right_eye_y=0;
        for(int i=36;i<=41;i++) left_eye_y += lr.landmarks[i].y;
        left_eye_y /= 6.0f;
        for(int i=42;i<=47;i++) right_eye_y += lr.landmarks[i].y;
        right_eye_y /= 6.0f;
        float diff = left_eye_y - right_eye_y;
        if(diff > 6.0f) return "tilt_right";
        if(diff < -6.0f) return "tilt_left";
    }
    return "unknown";
}
