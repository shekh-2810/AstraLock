# AstraLock Roadmap

Dates are intentionally omitted — features ship when stable.

---

## v0.1 (legacy — deprecated)

- LBPH-based face recognition
- PAM integration for login / sudo
- systemd-managed daemon, netcat IPC

**Status:** Tagged `v0.1.0` — superseded by v2.0

---

## v2.0 (previous)

- Full ONNX ArcFace pipeline (`w600k_mbf.onnx`, MobileNet backbone)
- RetinaFace detector via `FaceDetectorYN`
- Landmark-based face alignment (5-point → 112×112 ArcFace canonical)
- C++ enrollment — no Python dependency
- IPC v2 protocol (versioned JSON, typed error codes)
- Top-3 cosine distance averaging, threshold `0.30`
- PAM enabled for `sudo` and `login`
- Models hosted as GitHub release assets

---

## v2.1 (previous)
- ONNX include path handles both apt and manual installs via __has_include
- Config file (/etc/facelock/facelock.conf) now correctly read at daemon startup
- ONNX session caching — model loaded once, shared via mutex-protected singleton with 2 warmup inferences
- Enrollment quality checks — Laplacian blur detection and brightness range validation
- Structured audit logging to LOG_AUTHPRIV with event, user, ok, score, threshold fields
- CAMERA_DEVICE=N config option — switch to any /dev/videoN (e.g. IR camera) without recompiling
- Error responses now include a hint field with actionable guidance
- Automatic PAM injection for sudo, login, and active display managers (LightDM, GDM3, SDDM) at install time
- AppArmor drop-in auto-installed for nc.openbsd socket access
- Removed legacy LBPH code


---

## v3 (released)

- IPC v3 protocol — versioned UNIX socket with thread pool for concurrent auth requests
- Multi-user support — enroll and manage multiple users independently with isolated embeddings
- Per-user threshold calibration — cosine distance threshold tuned to each user's environment at enrollment
- Liveness detection — blink detection via EAR and texture scoring to block photo/video spoofing
- Real-time PAM conversation feedback — live status messages ("Face detected", "Attempt 1/3", "Verification successful") during auth
- Expanded CLI — list, delete, and help commands added
- Username sanitisation fix — special characters (e.g. ') no longer break enrollment
- PAM feedback via pam_conversation
- version.h now generated at build time from version.h.in via CMake into `build/generated/`
- Streamlined codebase — removed redundant files and consolidated dependencies 
---

## v3.1 (planned)

- GStreamer fallback — fix camera access under systemd cgroup isolation
- Live enrollment preview with quality scores (brightness, blur)
- Arch Linux and Fedora one-command installer
- Unit tests for scoring, config parsing, and IPC edge cases
- Multi-angle enrollment for better real-world match rate

---

## Design Philosophy

- Local-first, offline by default
- No cloud dependency
- Transparent and auditable
- PAM remains authoritative — face auth is always `sufficient`, never `required`

