#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>

#include "facelock/daemon.h"
#include <spdlog/spdlog.h>

int main(int argc, char** argv) {
    // Hard-disable OpenCL / GPU paths (prevents OCL crashes)
    cv::ocl::setUseOpenCL(false);
    cv::setUseOptimized(false);
    cv::setNumThreads(1);

    spdlog::set_level(spdlog::level::info);

    facelock::DaemonConfig cfg;
    // TODO: parse CLI args / config file if needed

    facelock::Daemon daemon(cfg);
    return daemon.run();
}
