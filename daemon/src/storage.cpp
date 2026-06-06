#include "facelock/storage.h"

#include <filesystem>
#include <cstdio>
#include <cstring>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;
using namespace facelock;

// ============================================================
//  Internal helpers
// ============================================================

std::string facelock::storage_path(const std::string& data_dir,
                                   const std::string& user)
{
    return (fs::path(data_dir) / (user + "_emb.bin")).string();
}

// ============================================================
//  storage_save
// ============================================================

bool facelock::storage_save(const std::string& data_dir,
                            const std::string& user,
                            const EmbeddingStore& store)
{
    if (store.embeddings.empty()) {
        spdlog::error("storage_save: no embeddings to save for '{}'", user);
        return false;
    }

    const uint32_t N = static_cast<uint32_t>(store.embeddings.size());
    const uint32_t D = static_cast<uint32_t>(store.embeddings[0].size());

    if (N > STORAGE_MAX_SAMPLES || D > STORAGE_MAX_DIM || D == 0) {
        spdlog::error("storage_save: out-of-range N={} D={} for '{}'", N, D, user);
        return false;
    }

    // Verify all embeddings have consistent dimension
    for (size_t i = 0; i < store.embeddings.size(); ++i) {
        if (store.embeddings[i].size() != D) {
            spdlog::error("storage_save: embedding {} has wrong dim {} (expected {})",
                          i, store.embeddings[i].size(), D);
            return false;
        }
    }

    fs::create_directories(data_dir);
    const std::string path = storage_path(data_dir, user);

    // Write to a temp file first, then rename — avoids partial writes
    const std::string tmp_path = path + ".tmp";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    if (!f) {
        spdlog::error("storage_save: cannot open '{}' for writing: {}",
                      tmp_path, strerror(errno));
        return false;
    }

    const uint32_t magic   = STORAGE_MAGIC;
    const uint32_t version = STORAGE_VERSION;
    const float    thresh  = store.threshold;

    bool ok = true;
    ok &= (fwrite(&magic,   sizeof(magic),   1, f) == 1);
    ok &= (fwrite(&version, sizeof(version), 1, f) == 1);
    ok &= (fwrite(&N,       sizeof(N),       1, f) == 1);
    ok &= (fwrite(&D,       sizeof(D),       1, f) == 1);
    ok &= (fwrite(&thresh,  sizeof(thresh),  1, f) == 1);

    for (const auto& emb : store.embeddings) {
        ok &= (fwrite(emb.data(), sizeof(float), D, f) == D);
        if (!ok) break;
    }

    fclose(f);

    if (!ok) {
        spdlog::error("storage_save: write error for '{}'", user);
        fs::remove(tmp_path);
        return false;
    }

    // Atomic replace
    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        spdlog::error("storage_save: rename failed for '{}': {}", user, ec.message());
        fs::remove(tmp_path);
        return false;
    }

    spdlog::debug("storage_save: saved {} embeddings (D={}, thresh={:.4f}) for '{}'",
                  N, D, thresh, user);
    return true;
}

// ============================================================
//  storage_load
// ============================================================

bool facelock::storage_load(const std::string& data_dir,
                            const std::string& user,
                            EmbeddingStore& store)
{
    store.embeddings.clear();
    store.dim       = 0;
    store.threshold = 0.30f;

    const std::string path = storage_path(data_dir, user);

    if (!fs::exists(path)) {
        return false;  // normal: user not enrolled — caller checks storage_exists first
    }

    // ---- File-size sanity before opening ----
    std::error_code ec;
    const uintmax_t file_size = fs::file_size(path, ec);
    if (ec) {
        spdlog::error("storage_load: cannot stat '{}': {}", path, ec.message());
        return false;
    }

    // Minimum: magic + version + N + D + threshold = 5 * 4 bytes = 20 bytes
    constexpr uintmax_t HEADER_SIZE = 5 * sizeof(uint32_t);  // magic+ver+N+D+thresh
    if (file_size < HEADER_SIZE) {
        spdlog::error("storage_load: file too small ({} bytes) for '{}'", file_size, user);
        return false;
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        spdlog::error("storage_load: cannot open '{}': {}", path, strerror(errno));
        return false;
    }

    // ---- Read and validate header ----
    uint32_t magic   = 0;
    uint32_t version = 0;
    uint32_t N       = 0;
    uint32_t D       = 0;
    float    thresh  = 0.30f;

    bool ok = true;
    ok &= (fread(&magic,   sizeof(magic),   1, f) == 1);
    ok &= (fread(&version, sizeof(version), 1, f) == 1);
    ok &= (fread(&N,       sizeof(N),       1, f) == 1);
    ok &= (fread(&D,       sizeof(D),       1, f) == 1);
    ok &= (fread(&thresh,  sizeof(thresh),  1, f) == 1);

    if (!ok) {
        spdlog::error("storage_load: header read failed for '{}'", user);
        fclose(f);
        return false;
    }

    if (magic != STORAGE_MAGIC) {
        spdlog::error("storage_load: bad magic 0x{:08X} for '{}' (expected 0x{:08X})",
                      magic, user, STORAGE_MAGIC);
        fclose(f);
        return false;
    }

    if (version != STORAGE_VERSION) {
        spdlog::error("storage_load: unsupported version {} for '{}'", version, user);
        fclose(f);
        return false;
    }

    if (N == 0 || N > STORAGE_MAX_SAMPLES) {
        spdlog::error("storage_load: invalid N={} for '{}'", N, user);
        fclose(f);
        return false;
    }

    if (D == 0 || D > STORAGE_MAX_DIM) {
        spdlog::error("storage_load: invalid D={} for '{}'", D, user);
        fclose(f);
        return false;
    }

    // ---- Exact file-size check ----
    const uintmax_t expected_size =
        HEADER_SIZE + static_cast<uintmax_t>(N) * D * sizeof(float);

    if (file_size != expected_size) {
        spdlog::error("storage_load: size mismatch for '{}': got {} bytes, expected {}",
                      user, file_size, expected_size);
        fclose(f);
        return false;
    }

    // ---- Read embeddings ----
    store.embeddings.resize(N, std::vector<float>(D));
    for (uint32_t i = 0; i < N; ++i) {
        if (fread(store.embeddings[i].data(), sizeof(float), D, f) != D) {
            spdlog::error("storage_load: short read at embedding {} for '{}'", i, user);
            fclose(f);
            store.embeddings.clear();
            return false;
        }
    }

    fclose(f);

    store.dim       = D;
    store.threshold = thresh;

    spdlog::debug("storage_load: loaded {} embeddings (D={}, thresh={:.4f}) for '{}'",
                  N, D, thresh, user);
    return true;
}

// ============================================================
//  storage_exists / storage_delete / storage_list
// ============================================================

bool facelock::storage_exists(const std::string& data_dir, const std::string& user)
{
    return fs::exists(storage_path(data_dir, user));
}

bool facelock::storage_delete(const std::string& data_dir, const std::string& user)
{
    const std::string path = storage_path(data_dir, user);
    std::error_code ec;
    bool removed = fs::remove(path, ec);
    if (ec) {
        spdlog::error("storage_delete: failed for '{}': {}", user, ec.message());
        return false;
    }
    return removed;
}

std::vector<std::string> facelock::storage_list(const std::string& data_dir)
{
    std::vector<std::string> users;
    std::error_code ec;

    if (!fs::is_directory(data_dir, ec)) return users;

    for (const auto& entry : fs::directory_iterator(data_dir, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        // Filename pattern: <username>_emb.bin
        constexpr std::string_view suffix = "_emb.bin";
        if (name.size() > suffix.size() &&
            name.substr(name.size() - suffix.size()) == suffix)
        {
            users.push_back(name.substr(0, name.size() - suffix.size()));
        }
    }
    return users;
}
