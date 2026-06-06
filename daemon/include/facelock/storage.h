#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ============================================================
//  AstraLock v3 — embedding storage
//
//  Binary format (all little-endian):
//    [0..3]   magic       uint32  0x464C454D  ("FLEM")
//    [4..7]   version     uint32  1
//    [8..11]  N           uint32  number of embeddings
//    [12..15] D           uint32  embedding dimension
//    [16..19] threshold   float   per-user calibrated cosine threshold
//    [20..]   embeddings  float[] N * D floats
//
//  Limits enforced on load:
//    N  <= STORAGE_MAX_SAMPLES   (256)
//    D  <= STORAGE_MAX_DIM       (1024)
//    file size must exactly equal header + N*D*sizeof(float)
//
//  These guards prevent OOB reads on a corrupted or tampered file.
// ============================================================

namespace facelock {

constexpr uint32_t STORAGE_MAGIC       = 0x464C454D;  // "FLEM"
constexpr uint32_t STORAGE_VERSION     = 1;
constexpr uint32_t STORAGE_MAX_SAMPLES = 256;
constexpr uint32_t STORAGE_MAX_DIM     = 1024;

struct EmbeddingStore {
    std::vector<std::vector<float>> embeddings;
    float                           threshold = 0.30f;  // per-user calibrated
    uint32_t                        dim       = 0;
};

// Returns false and logs reason on any integrity failure.
bool storage_save(const std::string& data_dir,
                  const std::string& user,
                  const EmbeddingStore& store);

// Returns false if file missing, corrupt, wrong magic, or bounds exceeded.
// On false, store is left empty and caller should fall back to PAM_IGNORE.
bool storage_load(const std::string& data_dir,
                  const std::string& user,
                  EmbeddingStore& store);

// Returns true if an embedding file exists for this user.
bool storage_exists(const std::string& data_dir, const std::string& user);

// Deletes the embedding file. Returns false if it didn't exist or removal failed.
bool storage_delete(const std::string& data_dir, const std::string& user);

// Returns a list of enrolled usernames found in data_dir.
std::vector<std::string> storage_list(const std::string& data_dir);

// Returns the full path for a user's embedding file (for logging/CLI use).
std::string storage_path(const std::string& data_dir, const std::string& user);

} // namespace facelock
