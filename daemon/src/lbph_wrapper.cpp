#include "facelock/lbph_wrapper.h"

#include <filesystem>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <stdexcept>

// cnpy for .npz reading
#include <cnpy.h>

using namespace facelock;
namespace fs = std::filesystem;

LBPHWrapper::~LBPHWrapper() = default;

/* ===================== math helpers ===================== */

static inline float l2_norm(const std::vector<float> &v) {
    double s = 0.0;
    for (float x : v) s += double(x) * double(x);
    return float(std::sqrt(s) + 1e-12);
}

static float cosine_distance(const std::vector<float> &a,
                             const std::vector<float> &b) {
    double dot = 0.0, na = 0.0, nb = 0.0;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        dot += double(a[i]) * double(b[i]);
        na  += double(a[i]) * double(a[i]);
        nb  += double(b[i]) * double(b[i]);
    }
    double denom = std::sqrt(na * nb) + 1e-12;
    return float(1.0 - (dot / denom));
}

/* ===================== LBP feature extractor ===================== */

static std::vector<float> compute_lbp_spatial_hist(
        const cv::Mat &gray_in,
        int grid = 8) {

    cv::Mat gray;
    if (gray_in.channels() == 3)
        cv::cvtColor(gray_in, gray, cv::COLOR_BGR2GRAY);
    else
        gray = gray_in;

    cv::Mat resized;
    cv::resize(gray, resized, cv::Size(200, 200));

    cv::Mat eq;
    cv::equalizeHist(resized, eq);

    const int h = eq.rows;
    const int w = eq.cols;

    cv::Mat lbp = cv::Mat::zeros(h, w, CV_8U);
    const int nbrs[8][2] = {
        {-1,-1},{0,-1},{1,-1},{1,0},
        {1,1},{0,1},{-1,1},{-1,0}
    };

    cv::Mat pad;
    cv::copyMakeBorder(eq, pad, 1,1,1,1, cv::BORDER_REPLICATE);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t center = pad.at<uint8_t>(y+1, x+1);
            uint8_t v = 0;
            for (int i = 0; i < 8; ++i) {
                int dx = nbrs[i][0];
                int dy = nbrs[i][1];
                if (pad.at<uint8_t>(y+1+dy, x+1+dx) >= center)
                    v |= (1 << i);
            }
            lbp.at<uint8_t>(y,x) = v;
        }
    }

    const int bins = 256;
    const int cell_h = h / grid;
    const int cell_w = w / grid;

    std::vector<float> feat;
    feat.reserve(grid * grid * bins);

    for (int gy = 0; gy < grid; ++gy) {
        for (int gx = 0; gx < grid; ++gx) {
            std::vector<int> hist(bins, 0);
            for (int y = gy * cell_h; y < (gy + 1) * cell_h; ++y)
                for (int x = gx * cell_w; x < (gx + 1) * cell_w; ++x)
                    hist[lbp.at<uint8_t>(y,x)]++;

            float sum = 0.f;
            for (int v : hist) sum += v;

            if (sum < 1e-6f) {
                feat.insert(feat.end(), bins, 0.f);
            } else {
                for (int v : hist)
                    feat.push_back(float(v) / sum);
            }
        }
    }

    float n = l2_norm(feat);
    for (float &v : feat) v /= n;
    return feat;
}

/* ===================== LOAD (NPZ: embeddings only) ===================== */

bool LBPHWrapper::load(const std::string &user,
                       const std::string &model_dir) {

    fs::path npz = fs::path(model_dir) / (user + "_lbph.npz");
    if (!fs::exists(npz)) {
        std::cerr << "LBPHWrapper: NPZ not found: " << npz << "\n";
        backend_ = "none";
        return false;
    }

    try {
        cnpy::npz_t archive = cnpy::npz_load(npz.string());

        if (!archive.count("embeddings"))
            throw std::runtime_error("missing 'embeddings'");

        cnpy::NpyArray arr = archive["embeddings"];

        if (arr.word_size != sizeof(float) || arr.shape.size() != 2)
            throw std::runtime_error("invalid embeddings array");

        const size_t N = arr.shape[0];
        const size_t D = arr.shape[1];
        const float *data = arr.data<float>();

        embeddings_.clear();
        embeddings_.reserve(N);

        for (size_t i = 0; i < N; ++i) {
            embeddings_.emplace_back(
                data + i * D,
                data + (i + 1) * D
            );
        }

        emb_dim_ = D;
        backend_ = "npz";

        std::cerr << "LBPHWrapper: loaded "
                  << N << " embeddings (dim=" << D << ")\n";
        return true;

    } catch (const std::exception &e) {
        std::cerr << "LBPHWrapper: NPZ load failed: "
                  << e.what() << "\n";
        backend_ = "none";
        return false;
    }
}

/* ===================== PREDICT ===================== */

std::optional<LBPHResult>
LBPHWrapper::predict(const cv::Mat &gray_crop, float threshold) {

    if (backend_ != "npz" || embeddings_.empty())
        return std::nullopt;

    std::vector<float> q = compute_lbp_spatial_hist(gray_crop, 8);

    float best = 1e9f;
    size_t best_i = 0;

    for (size_t i = 0; i < embeddings_.size(); ++i) {
        float d = cosine_distance(q, embeddings_[i]);
        if (d < best) {
            best = d;
            best_i = i;
        }
    }

    return LBPHResult{
        best <= threshold,
        best,
        std::to_string(best_i)
    };
}
