#!/usr/bin/env python3
"""
Compute LBP-spatial hist embeddings from a folder of face PNGs (raw enroll output)
and write <user>_emb.bin compatible with daemon/src/storage.cpp

Usage:
  python3 scripts/make_emb_bin_from_raw.py /var/lib/facelock/alice /var/lib/facelock alice
This will write /var/lib/facelock/alice_emb.bin
"""
import sys
import os
import cv2
import numpy as np
import struct
from glob import glob

def compute_lbp_spatial_hist(gray, grid=8):
    # gray: CV_8U numpy array
    # resize to 200x200 to match C++ implementation
    resized = cv2.resize(gray, (200,200), interpolation=cv2.INTER_LINEAR)
    eq = cv2.equalizeHist(resized)
    h, w = eq.shape
    # pad by 1 replicate
    pad = cv2.copyMakeBorder(eq, 1,1,1,1, cv2.BORDER_REPLICATE)
    nbrs = [(-1,-1),(0,-1),(1,-1),(1,0),(1,1),(0,1),(-1,1),(-1,0)]
    out = np.zeros((h,w), dtype=np.uint8)
    for y in range(h):
        for x in range(w):
            center = pad[y+1, x+1]
            v = 0
            for i,(dx,dy) in enumerate(nbrs):
                nb = pad[y+1+dy, x+1+dx]
                if nb >= center:
                    v |= (1 << i)
            out[y,x] = v
    nbins = 256
    cell_h = h // grid
    cell_w = w // grid
    feats = []
    for gy in range(grid):
        for gx in range(grid):
            y0 = gy * cell_h
            x0 = gx * cell_w
            patch = out[y0:y0+cell_h, x0:x0+cell_w].ravel()
            hist = np.bincount(patch, minlength=nbins).astype(np.float32)
            s = hist.sum()
            if s > 0:
                hist /= s
            feats.append(hist)
    feats = np.concatenate(feats).astype(np.float32)  # length = grid*grid*256
    # L2 normalize
    norm = np.linalg.norm(feats) + 1e-12
    feats /= norm
    return feats

def main():
    if len(sys.argv) != 4:
        print("Usage: {} <raw_images_dir> <out_data_dir> <user>".format(sys.argv[0]))
        print("Example: {} /var/lib/facelock/alice /var/lib/facelock alice".format(sys.argv[0]))
        sys.exit(2)

    raw_dir = sys.argv[1]
    out_data_dir = sys.argv[2]
    user = sys.argv[3]

    if not os.path.isdir(raw_dir):
        print("Raw images dir not found:", raw_dir)
        sys.exit(2)

    # find images (sorted)
    imgs = sorted(glob(os.path.join(raw_dir, "*.png")) + glob(os.path.join(raw_dir, "*.jpg")))
    if not imgs:
        print("No images found in", raw_dir)
        sys.exit(2)

    embs = []
    names = []
    for p in imgs:
        img = cv2.imread(p, cv2.IMREAD_GRAYSCALE)
        if img is None:
            print("Failed to load:", p, " — skipping")
            continue
        feats = compute_lbp_spatial_hist(img, grid=8)
        embs.append(feats)
        names.append(os.path.basename(p))

    if not embs:
        print("No valid embeddings produced.")
        sys.exit(2)

    embs = np.stack(embs)  # shape (N, D)
    N, D = embs.shape
    out_path = os.path.join(out_data_dir, f"{user}_emb.bin")
    print(f"Writing {N} embeddings (dim={D}) -> {out_path}")

    # write binary matching storage.cpp: uint32 n, uint32 d, then floats (row-major)
    with open(out_path, "wb") as f:
        f.write(struct.pack("<I", N))
        f.write(struct.pack("<I", D))
        # ensure little-endian float32
        embs.astype(np.float32).tofile(f)

    print("Done.")

if __name__ == "__main__":
    main()
