#include "facelock/storage.h"
#include <fstream>
#include <filesystem>

using namespace facelock;
namespace fs = std::filesystem;

bool facelock::save_embeddings(const std::string& data_dir, const std::string& user, const std::vector<std::vector<float>>& embs) {
    fs::path dir(data_dir);
    fs::create_directories(dir);
    fs::path out = dir / (user + "_emb.bin");
    std::ofstream ofs(out, std::ios::binary);
    if(!ofs) return false;
    uint32_t n = static_cast<uint32_t>(embs.size());
    uint32_t d = embs.empty() ? 0 : static_cast<uint32_t>(embs[0].size());
    ofs.write(reinterpret_cast<const char*>(&n), sizeof(n));
    ofs.write(reinterpret_cast<const char*>(&d), sizeof(d));
    for(const auto &v : embs) ofs.write(reinterpret_cast<const char*>(v.data()), sizeof(float)*d);
    ofs.close();
    return true;
}

std::vector<std::vector<float>> facelock::load_embeddings(const std::string& data_dir, const std::string& user) {
    std::vector<std::vector<float>> out;
    fs::path in = fs::path(data_dir) / (user + "_emb.bin");
    if(!fs::exists(in)) return out;
    std::ifstream ifs(in, std::ios::binary);
    uint32_t n,d; ifs.read(reinterpret_cast<char*>(&n), sizeof(n)); ifs.read(reinterpret_cast<char*>(&d), sizeof(d));
    out.resize(n, std::vector<float>(d));
    for(uint32_t i=0;i<n;i++) ifs.read(reinterpret_cast<char*>(out[i].data()), sizeof(float)*d);
    return out;
}
