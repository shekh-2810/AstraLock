#pragma once
#include <string>
#include <vector>

namespace facelock {

// Storage interface: persist/load embeddings or raw training data.
// In production this should use encrypted storage (SQLCipher or similar).
bool save_embeddings(const std::string& data_dir, const std::string& user, const std::vector<std::vector<float>>& embs);
std::vector<std::vector<float>> load_embeddings(const std::string& data_dir, const std::string& user);

} // namespace facelock

