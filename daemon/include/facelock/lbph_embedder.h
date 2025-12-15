#pragma once
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <nlohmann/json.hpp>

namespace facelock {

struct LBPHModelMeta {
    std::string user;
    int grid = 8;
    int dim = 0;
    int n_samples = 0;
    float threshold = 0.30f;
    std::vector<std::string> filenames;
};

class LBPHEmbedder {
public:
    LBPHEmbedder() = default;
    ~LBPHEmbedder() = default;

    // Load embeddings.npy and metadata.json from model_dir (folder path)
    bool load_model(const std::string &model_dir, const std::string &user);

    // Compute embedding from a grayscale Mat (expects CV_8U)
    std::vector<float> embed_from_gray(const cv::Mat &gray) const;

    // Compute best (min) cosine-distance between query and stored embeddings
    // Returns pair<match(bool), best_score(float)>
    std::pair<bool, float> match(const cv::Mat &gray) const;

    const LBPHModelMeta &meta() const { return meta_; }

private:
    // internal helpers (match python implementation)
    cv::Mat preprocess_gray(const cv::Mat &gray) const;
    std::vector<float> compute_lbp_embedding(const cv::Mat &gray) const;
    float cosine_distance(const std::vector<float> &a, const float *B, size_t stride, size_t n) const;

    LBPHModelMeta meta_;
    std::vector<float> embeddings_flat_; // row-major (N x D)
    size_t emb_dim_ = 0;
    size_t emb_count_ = 0;
};
} // namespace facelock
