#include "facelock/lbph_embedder.h"
#include <cnpy.h>
#include <fstream>
#include <cmath>
#include <algorithm>

using namespace facelock;
using json = nlohmann::json;

bool LBPHEmbedder::load_model(const std::string &model_dir, const std::string &user) {
    // model_dir expected to contain <user>_embeddings.npy and <user>_lbph.json
    std::string npy_path = model_dir + "/" + user + "_embeddings.npy";
    std::string json_path = model_dir + "/" + user + "_lbph.json";

    // read json metadata
    std::ifstream in(json_path);
    if (!in.good()) return false;
    json j; in >> j;
    meta_.user = j.value("user", user);
    meta_.grid = j.value("grid", 8);
    meta_.dim = j.value("dim", 0);
    meta_.n_samples = j.value("n_samples", 0);
    meta_.threshold = j.value("threshold", 0.30f);
    if (j.contains("filenames")) {
        for (auto &f : j["filenames"]) meta_.filenames.push_back(f.get<std::string>());
    }

    // load numpy
    cnpy::NpyArray arr = cnpy::npy_load(npy_path);
    // cnpy NpyArray exposes members (not functions)
    if (arr.word_size != sizeof(float) || arr.fortran_order) {
        // we expect float32 row-major
        // warn but continue
    }
    std::vector<size_t> shape = arr.shape;
    if (shape.size() != 2) return false;
    emb_count_ = shape[0];
    emb_dim_ = shape[1];
    // copy into embeddings_flat_
    float *data = arr.data<float>();
    embeddings_flat_.assign(data, data + (emb_count_ * emb_dim_));
    return true;
}

cv::Mat LBPHEmbedder::preprocess_gray(const cv::Mat &gray) const {
    // mirror Python: resize -> equalizeHist
    cv::Mat resized;
    cv::resize(gray, resized, cv::Size(200,200));
    cv::Mat eq;
    cv::equalizeHist(resized, eq);
    return eq;
}

std::vector<float> LBPHEmbedder::compute_lbp_embedding(const cv::Mat &gray) const {
    // expects 200x200 CV_8U
    int h = gray.rows, w = gray.cols;
    cv::Mat out = cv::Mat::zeros(h, w, CV_8U);
    // neighbor offsets
    const int nbrs[8][2] = {{-1,-1},{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0}};
    // pad
    cv::Mat pad;
    cv::copyMakeBorder(gray, pad, 1,1,1,1, cv::BORDER_REPLICATE);
    for (int y=0;y<h;y++){
        for (int x=0;x<w;x++){
            uint8_t center = pad.at<uint8_t>(y+1,x+1);
            unsigned char val = 0;
            for (int i=0;i<8;i++){
                int dx = nbrs[i][0], dy = nbrs[i][1];
                uint8_t nb = pad.at<uint8_t>(y+1+dy, x+1+dx);
                if (nb >= center) val |= (1 << i);
            }
            out.at<uint8_t>(y,x) = val;
        }
    }
    // spatial hist
    int grid = meta_.grid > 0 ? meta_.grid : 8;
    int cell_h = h / grid;
    int cell_w = w / grid;
    const int nbins = 256;
    std::vector<float> feats;
    feats.reserve(grid * grid * nbins);
    for (int gy=0; gy<grid; ++gy) {
        for (int gx=0; gx<grid; ++gx) {
            int y0 = gy*cell_h, x0 = gx*cell_w;
            std::vector<int> hist(nbins);
            for (int yy=y0; yy<y0+cell_h; ++yy) {
                for (int xx=x0; xx<x0+cell_w; ++xx) {
                    uint8_t v = out.at<uint8_t>(yy,xx);
                    hist[v]++;
                }
            }
            float sum = 0.0f;
            for (int k=0;k<nbins;++k) sum += hist[k];
            if (sum <= 0.0f) {
                for (int k=0;k<nbins;++k) feats.push_back(0.0f);
            } else {
                for (int k=0;k<nbins;++k) feats.push_back(hist[k] / sum);
            }
        }
    }
    // L2-normalize
    float s = 0.0f;
    for (auto &v : feats) s += v * v;
    s = std::sqrt(s) + 1e-12f;
    for (auto &v : feats) v /= s;
    return feats;
}

std::vector<float> LBPHEmbedder::embed_from_gray(const cv::Mat &gray) const {
    cv::Mat pre = preprocess_gray(gray);
    return compute_lbp_embedding(pre);
}

float LBPHEmbedder::cosine_distance(const std::vector<float> &a, const float *B, size_t stride, size_t n) const {
    // a length = emb_dim_, B pointer points to flattened rows; stride == emb_dim_
    float an = 0.0f;
    for (size_t i=0;i<n;i++) an += a[i]*a[i];
    an = std::sqrt(an) + 1e-12f;
    // compute dot/Bnorm quickly
    float best = 1.0f;
    for (size_t r=0;r<emb_count_;++r) {
        const float *row = B + r * stride;
        float dot = 0.0f, bn = 0.0f;
        for (size_t i=0;i<n;++i) {
            dot += a[i] * row[i];
            bn += row[i] * row[i];
        }
        bn = std::sqrt(bn) + 1e-12f;
        float cos = dot / (an * bn);
        if (cos < -1.0f) cos = -1.0f;
        if (cos > 1.0f) cos = 1.0f;
        float dist = 1.0f - cos;
        if (dist < best) best = dist;
    }
    return best;
}

std::pair<bool, float> LBPHEmbedder::match(const cv::Mat &gray) const {
    if (emb_dim_ == 0 || emb_count_ == 0) return {false, 1.0f};
    std::vector<float> q = embed_from_gray(gray);
    float best = cosine_distance(q, embeddings_flat_.data(), emb_dim_, emb_dim_);
    bool ok = (best <= meta_.threshold);
    return {ok, best};
}
