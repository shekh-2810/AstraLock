#!/usr/bin/env python3
"""
Pure LBPH trainer (C++ SAFE, NO cv2.face)

Input:
  /var/lib/facelock/<user>/*.png

Output:
  /var/lib/facelock/<user>_lbph.npz

NPZ contents (NUMERIC ONLY, UNCOMPRESSED):
  - embeddings: float32 (N, D)

IMPORTANT:
  - Uses np.savez (NOT savez_compressed)
  - Fully compatible with cnpy (C++)
"""

import argparse
import pathlib
import sys
import numpy as np
import cv2

# -------------------- args --------------------

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("user")
    p.add_argument("--data-dir", default="/var/lib/facelock")
    p.add_argument("--grid", type=int, default=8)
    return p.parse_args()

# -------------------- LBP --------------------

def lbp_gray(img):
    h, w = img.shape
    out = np.zeros_like(img, dtype=np.uint8)

    nbrs = [
        (-1, -1), (0, -1), (1, -1), (1,  0),
        (1,  1), (0,  1), (-1, 1), (-1, 0)
    ]

    pad = np.pad(img, 1, mode="edge")

    for y in range(h):
        for x in range(w):
            c = pad[y + 1, x + 1]
            v = 0
            for i, (dx, dy) in enumerate(nbrs):
                if pad[y + 1 + dy, x + 1 + dx] >= c:
                    v |= (1 << i)
            out[y, x] = v

    return out

# -------------------- spatial histogram --------------------

def spatial_hist(lbp, grid):
    h, w = lbp.shape
    ch, cw = h // grid, w // grid

    feats = []

    for gy in range(grid):
        for gx in range(grid):
            cell = lbp[
                gy * ch : (gy + 1) * ch,
                gx * cw : (gx + 1) * cw
            ]

            hist, _ = np.histogram(cell, bins=256, range=(0, 256))
            hist = hist.astype(np.float32)

            s = hist.sum()
            if s > 0:
                hist /= s

            feats.append(hist)

    feat = np.concatenate(feats)
    feat /= (np.linalg.norm(feat) + 1e-12)

    return feat.astype(np.float32)

# -------------------- main --------------------

def main():
    args = parse_args()

    user_dir = pathlib.Path(args.data_dir) / args.user
    imgs = sorted(user_dir.glob("*.png"))

    if not imgs:
        print("No images found in", user_dir)
        return 1

    embs = []

    for fp in imgs:
        im = cv2.imread(str(fp), cv2.IMREAD_GRAYSCALE)
        if im is None:
            continue

        im = cv2.resize(im, (200, 200))
        lbp = lbp_gray(im)
        feat = spatial_hist(lbp, args.grid)
        embs.append(feat)

    if not embs:
        print("No valid images")
        return 2

    embs = np.stack(embs).astype(np.float32)

    out = pathlib.Path(args.data_dir) / f"{args.user}_lbph.npz"

    np.savez(out, embeddings=embs)

    print("Saved", out, "shape:", embs.shape)
    return 0

# --------------------

if __name__ == "__main__":
    sys.exit(main())
