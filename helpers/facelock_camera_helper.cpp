#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <cstring>

static const char* DETECTOR_MODEL =
    "/usr/share/facelock/models/retinaface.onnx";

// ArcFace canonical 112x112 landmark positions
static const cv::Point2f ARCFACE_DST[5] = {
    {38.2946f, 51.6963f},  // left eye
    {73.5318f, 51.5014f},  // right eye
    {56.0252f, 71.7366f},  // nose
    {41.5493f, 92.3655f},  // left mouth
    {70.7299f, 92.2041f}   // right mouth
};

enum class Mode { GRAY200, BGR112 };

static Mode parse_mode(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (std::strcmp(argv[i+1], "gray200") == 0) return Mode::GRAY200;
            if (std::strcmp(argv[i+1], "bgr112")  == 0) return Mode::BGR112;
        }
    }
    return Mode::BGR112;
}

static int parse_camera(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            return std::atoi(argv[i+1]);
        }
    }
    return 0; // default: /dev/video0
}

// Align face to ArcFace canonical positions using landmarks
static cv::Mat align_face(const cv::Mat& frame, const cv::Mat& faces, int idx) {
    cv::Point2f src[5] = {
        {faces.at<float>(idx, 4),  faces.at<float>(idx, 5)},   // left eye
        {faces.at<float>(idx, 6),  faces.at<float>(idx, 7)},   // right eye
        {faces.at<float>(idx, 8),  faces.at<float>(idx, 9)},   // nose
        {faces.at<float>(idx, 10), faces.at<float>(idx, 11)},  // left mouth
        {faces.at<float>(idx, 12), faces.at<float>(idx, 13)}   // right mouth
    };

    cv::Mat transform = cv::estimateAffinePartial2D(
        std::vector<cv::Point2f>(src, src + 5),
        std::vector<cv::Point2f>(ARCFACE_DST, ARCFACE_DST + 5)
    );

    cv::Mat aligned;
    cv::warpAffine(frame, aligned, transform, {112, 112},
                   cv::INTER_LINEAR, cv::BORDER_REFLECT);
    return aligned;
}

int main(int argc, char** argv) {
    Mode mode = parse_mode(argc, argv);
    int  cam  = parse_camera(argc, argv);

    cv::VideoCapture cap(cam);
    if (!cap.isOpened()) return 1;

    cap.set(cv::CAP_PROP_FRAME_WIDTH,  640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

    cv::Ptr<cv::FaceDetectorYN> detector = cv::FaceDetectorYN::create(
        DETECTOR_MODEL, "", {640, 480},
        0.6f, 0.3f, 5000
    );
    if (detector.empty()) return 2;

    // warmup
    cv::Mat junk;
    for (int i = 0; i < 10; ++i) {
        cap >> junk;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    auto start = std::chrono::steady_clock::now();

    while (true) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty()) continue;

        cv::Mat faces;
        detector->detect(frame, faces);

        if (faces.rows > 0) {
            // pick highest confidence face
            int best = 0;
            float best_score = faces.at<float>(0, 14);
            for (int i = 1; i < faces.rows; ++i) {
                float s = faces.at<float>(i, 14);
                if (s > best_score) { best_score = s; best = i; }
            }

            if (mode == Mode::BGR112) {
                cv::Mat aligned = align_face(frame, faces, best);
                std::cout.write(
                    reinterpret_cast<char*>(aligned.data),
                    112 * 112 * 3
                );
            } else {
                // legacy gray200 — no alignment
                int x = std::max(0, (int)faces.at<float>(best, 0));
                int y = std::max(0, (int)faces.at<float>(best, 1));
                int w = std::min((int)faces.at<float>(best, 2), frame.cols - x);
                int h = std::min((int)faces.at<float>(best, 3), frame.rows - y);
                if (w > 0 && h > 0) {
                    cv::Mat gray;
                    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
                    cv::Mat crop = gray(cv::Rect(x,y,w,h)).clone();
                    cv::resize(crop, crop, {200, 200});
                    std::cout.write(reinterpret_cast<char*>(crop.data), 200*200);
                }
            }
            return 0;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > 2000) break;
    }

    return 3;
}