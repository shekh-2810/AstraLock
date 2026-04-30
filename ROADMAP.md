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

## v2.1 (released)

**Bug fixes (from user report):**
- ONNX Runtime include path: handles both apt (`<onnxruntime/…>`) and manual install paths via `__has_include`
- Config file is now actually read from `/etc/facelock/facelock.conf` at daemon startup
- AppArmor drop-in auto-installed for `nc.openbsd` socket access

**Performance:**
- ONNX session caching — model loaded once at daemon start, shared across all requests via mutex-protected singleton; warmup runs 2 dummy inferences on startup

**Enrollment quality:**
- Laplacian variance blur detection — rejects frames below sharpness threshold
- Brightness range check — rejects near-black or overexposed frames
- Retry guidance printed to log every 5 quality rejects

**Logging & audit:**
- All auth and enroll events written to `LOG_AUTHPRIV` syslog with structured fields: `event`, `user`, `ok`, `score`, `threshold`
- Same events also logged via spdlog at appropriate levels

**Config & camera:**
- `CAMERA_DEVICE=N` in `/etc/facelock/facelock.conf` selects any `/dev/videoN` — use for IR cameras without recompiling
- `--camera N` flag added to `facelock-camera-helper`

**Error messages:**
- Every error response now includes a `hint` field with actionable guidance
- e.g. `not_enrolled → "Run: facelock enroll <user>"`, `no_face → "Position your face in front of the camera"`

**PAM integration (automatic):**
- Installer automatically adds `auth sufficient pam_facelock.so` to:
  - `/etc/pam.d/sudo`
  - `/etc/pam.d/login`
  - `/etc/pam.d/lightdm` + `lightdm-greeter` (if LightDM active)
  - `/etc/pam.d/gdm-password` (if GDM3 active)
  - `/etc/pam.d/sddm` (if SDDM active)
- Uninstaller cleanly removes all injected lines
- Face unlock at lock screen and boot login now works out of the box

**Code cleanup:**
- Removed legacy LBPH files: `lbph_wrapper.{h,cpp}`, `lbph_embedder.{h,cpp}`

---

## v3 (planned)

- **Liveness detection** — motion/texture-based anti-spoofing against photos and video replay
- **Unit tests** — scoring logic, error paths, config parsing
- **Arch / Fedora support** — packaging and dependency detection for non-Debian distros
- **pam_conversation feedback** — real-time "face detected / try again" messages to the PAM conversation handler

---

## Design Philosophy

- Local-first, offline by default
- No cloud dependency
- Transparent and auditable
- PAM remains authoritative — face auth is always `sufficient`, never `required`
