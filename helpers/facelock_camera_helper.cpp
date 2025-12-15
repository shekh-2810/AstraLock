#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <thread>

static const char* CASCADE =
    "/usr/share/opencv4/haarcascades/haarcascade_frontalface_default.xml";

int main() {
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        return 1;
    }

    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

    cv::CascadeClassifier cascade;
    if (!cascade.load(CASCADE)) {
        return 2;
    }

    // warmup
    cv::Mat junk;
    for (int i = 0; i < 10; ++i) {
        cap >> junk;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    auto start = std::chrono::steady_clock::now();

    while (true) {
        cv::Mat frame, gray;
        cap >> frame;
        if (frame.empty()) continue;

        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        std::vector<cv::Rect> faces;

        cascade.detectMultiScale(gray, faces, 1.1, 4, 0, {80, 80});

        if (!faces.empty()) {
            auto r = *std::max_element(
                faces.begin(), faces.end(),
                [](auto &a, auto &b){ return a.area() < b.area(); });

            cv::Mat crop = gray(r).clone();
            cv::resize(crop, crop, {200, 200});

            // RAW grayscale output to stdout
            std::cout.write(
                reinterpret_cast<char*>(crop.data),
                crop.total()
            );
            return 0;
        }

        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > 1500)
            break;
    }

    return 3;
}
