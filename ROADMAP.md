# AstraLock Roadmap

Dates are intentionally omitted — features ship when stable.

---

## v0.1 — Initial Prototype (released)
- Daemon architecture + PAM integration
- LBPH-based face recognition (OpenCV)
- systemd service + UNIX socket IPC
- Basic CLI (enroll, verify, test)

---

## v0.2 — Stabilization (released)
- Debian/Ubuntu/Kali support stabilized
- netcat-openbsd compatibility enforced
- Dependency detection improved
- Installer reliability fixes

---

## v2.0 — ONNX ArcFace Pipeline (current)
- Replaced LBPH with ArcFace ONNX embeddings (w600k_mbf)
- Face alignment using FaceDetectorYN landmarks (112×112 canonical)
- RetinaFace detector — better low-light and angle handling
- Enrollment runs entirely in C++ daemon (no Python dependency)
- IPC protocol v2 — versioned JSON with typed error codes
- Top-3 cosine distance averaging for stable auth scoring
- Threshold tuned to 0.30 cosine distance

---

## v2.1 — Planned
- Socket permissions fix (non-root CLI access)
- ONNXWrapper instance caching (reduce per-auth latency)
- Remove LBPH legacy code entirely
- Arch Linux + Fedora support
- Config file parsing in daemon (read facelock.conf at runtime)

---

## Design Philosophy
- Local-first, offline by default
- No cloud dependency
- Transparent and auditable
- PAM remains authoritative — password fallback always available
