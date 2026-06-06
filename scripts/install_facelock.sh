#!/usr/bin/env bash
# ============================================================
#  AstraLock v3 — installer
# ============================================================
set -euo pipefail

# ── Argument check ────────────────────────────────────────────
if [ -z "${1-}" ]; then
  echo "Usage: sudo $0 <username>"
  exit 1
fi
USER_NAME="$1"

if [ "$(id -u)" -ne 0 ]; then
  echo "[!] This script must be run as root (sudo $0 $USER_NAME)"
  exit 1
fi

# ── Constants ─────────────────────────────────────────────────
ONNX_VER="1.17.3"
MODEL_DIR="/usr/share/facelock/models"
MODEL_PATH="$MODEL_DIR/w600k_mbf.onnx"
DETECTOR_PATH="$MODEL_DIR/retinaface.onnx"
RELEASE_BASE="https://github.com/shekh-2810/AstraLock/releases/download/v3.0"
MODEL_URL="$RELEASE_BASE/w600k_mbf.onnx"
DETECTOR_URL="$RELEASE_BASE/retinaface.onnx"

PAM_SO_DIR="/lib/x86_64-linux-gnu/security"

# ── Helpers ───────────────────────────────────────────────────
info()  { echo "[*] $*"; }
warn()  { echo "[!] $*"; }
die()   { echo "[✗] $*"; exit 1; }
ok()    { echo "[✓] $*"; }

# Insert a PAM rule at the top of a service file (idempotent)
pam_insert() {
  local svc="$1"
  local rule="auth sufficient pam_facelock.so"
  if [ ! -f "$svc" ]; then
    warn "$svc not found — skipping"
    return
  fi
  if grep -qF "pam_facelock.so" "$svc"; then
    info "PAM rule already present in $svc — skipping"
  else
    sed -i "1s|^|${rule}\n|" "$svc"
    ok "PAM rule added to $svc"
  fi
}

# Check if a package is available in apt without installing it
apt_available() { apt-cache show "$1" >/dev/null 2>&1; }

# ── Dependencies ──────────────────────────────────────────────
info "Installing build dependencies"
apt-get update -qq
apt-get install -y \
  cmake ninja-build g++ \
  libpam0g-dev libaudit-dev \
  libopencv-dev \
  libspdlog-dev \
  nlohmann-json3-dev \
  libsystemd-dev \
  pkg-config \
  pamtester jq wget

# ── ONNX Runtime ──────────────────────────────────────────────
info "Installing ONNX Runtime"
if apt_available libonnxruntime-dev && apt-get install -y libonnxruntime-dev; then
  ok "ONNX Runtime installed via apt"
else
  info "libonnxruntime-dev not in apt — installing from tgz (v${ONNX_VER})"
  ONNX_TGZ="/tmp/onnxruntime-${ONNX_VER}.tgz"
  wget -q --show-progress \
    "https://github.com/microsoft/onnxruntime/releases/download/v${ONNX_VER}/onnxruntime-linux-x64-${ONNX_VER}.tgz" \
    -O "$ONNX_TGZ"
  tar -xzf "$ONNX_TGZ" -C /usr/local --strip-components=1
  ldconfig
  rm -f "$ONNX_TGZ"
  ok "ONNX Runtime ${ONNX_VER} installed to /usr/local"
fi

# ── Models ────────────────────────────────────────────────────
info "Ensuring model directory exists"
mkdir -p "$MODEL_DIR"

if [ ! -f "$MODEL_PATH" ]; then
  info "Downloading ArcFace model (w600k_mbf.onnx) ..."
  wget -q --show-progress "$MODEL_URL" -O "$MODEL_PATH" \
    || die "Failed to download ArcFace model. Check network or GitHub release."
  ok "ArcFace model downloaded"
else
  info "ArcFace model already present — skipping"
fi

if [ ! -f "$DETECTOR_PATH" ]; then
  info "Downloading RetinaFace detector model ..."
  wget -q --show-progress "$DETECTOR_URL" -O "$DETECTOR_PATH" \
    || die "Failed to download RetinaFace model. Check network or GitHub release."
  ok "RetinaFace model downloaded"
else
  info "RetinaFace detector already present — skipping"
fi

# ── Directory structure ───────────────────────────────────────
info "Creating runtime directories"
mkdir -p /var/lib/facelock /etc/facelock
# /run/facelock is created by the systemd RuntimeDirectory= directive

# ── Config file ───────────────────────────────────────────────
CONFIG_DEST="/etc/facelock/facelock.conf"
CONFIG_SRC="packaging/config/facelock.conf"

if [ ! -f "$CONFIG_DEST" ]; then
  if [ -f "$CONFIG_SRC" ]; then
    install -m 644 "$CONFIG_SRC" "$CONFIG_DEST"
    ok "Config installed to $CONFIG_DEST"
  else
    # Write a complete v3 default config if the source file isn't present
    cat > "$CONFIG_DEST" <<'CONF'
# ============================================================
#  AstraLock v3 — /etc/facelock/facelock.conf
#  All settings are optional. Uncomment and edit as needed.
#  Restart the daemon after changes:
#    systemctl restart facelockd
# ============================================================

# ── IPC ──────────────────────────────────────────────────────
# SOCKET_PATH=/run/facelock/facelock.sock

# ── Storage ───────────────────────────────────────────────────
# DATA_DIR=/var/lib/facelock/

# ── Models ────────────────────────────────────────────────────
# ONNX_MODEL_PATH=/usr/share/facelock/models/w600k_mbf.onnx
# DETECTOR_MODEL_PATH=/usr/share/facelock/models/retinaface.onnx

# ── Camera ───────────────────────────────────────────────────
# Run: ls /dev/video*  to identify your camera.
# For IR cameras, this is usually 2 or higher.
# CAMERA_DEVICE=0

# ── Scoring ───────────────────────────────────────────────────
# Global fallback threshold (0.0 = strictest, 1.0 = most permissive).
# Per-user threshold is calibrated at enroll time and takes priority.
# ONNX_THRESHOLD=0.30

# ── Enrollment ────────────────────────────────────────────────
# ENROLL_TARGET=20
# ENROLL_MIN=10
# ENROLL_THRESHOLD_K=1.5

# ── Liveness (anti-spoofing) ──────────────────────────────────
# Set LIVENESS_ENABLED=false to disable (development/testing only).
# LIVENESS_ENABLED=true
# LIVENESS_TEXTURE_MIN=0.15
# LIVENESS_BLINK_FRAMES=12
# LIVENESS_EAR_THRESHOLD=0.22

# ── Performance ───────────────────────────────────────────────
# IPC_THREADS=4
# CAMERA_WARMUP_FRAMES=5
CONF
    ok "Default config written to $CONFIG_DEST"
  fi
  info "  → Set CAMERA_DEVICE=N for your camera (ls /dev/video*)"
  info "  → Restart after changes: systemctl restart facelockd"
else
  info "Config already exists at $CONFIG_DEST — not overwriting"
  info "  → New v3 keys: LIVENESS_*, ENROLL_THRESHOLD_K, IPC_THREADS"
  info "     See packaging/config/facelock.conf for documentation"
fi

# ── AppArmor drop-in ──────────────────────────────────────────
if command -v apparmor_parser >/dev/null 2>&1; then
  info "Installing AppArmor drop-in (socket access for PAM)"
  APPARMOR_SRC="packaging/apparmor"
  if [ -d "$APPARMOR_SRC" ]; then
    mkdir -p /etc/apparmor.d/local
    for profile in "$APPARMOR_SRC"/*; do
      [ -f "$profile" ] || continue
      install -m 644 "$profile" /etc/apparmor.d/local/
      # Reload if the base profile is already loaded
      base="/etc/apparmor.d/$(basename "$profile")"
      if [ -f "$base" ]; then
        apparmor_parser -r "$base" 2>/dev/null || true
      fi
    done
    ok "AppArmor drop-ins installed"
  fi
else
  info "AppArmor not active — skipping drop-in"
fi

# ── Build ─────────────────────────────────────────────────────
info "Cleaning previous builds"
rm -rf build build-pam

info "Building daemon (v3)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
cmake --install build
ldconfig

info "Building PAM module"
cmake -S pam -B build-pam -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-pam -j"$(nproc)"
install -m 755 build-pam/pam_facelock.so "$PAM_SO_DIR/"
ok "pam_facelock.so installed to $PAM_SO_DIR/"

# ── CLI ───────────────────────────────────────────────────────
info "Installing facelock CLI"
install -m 755 scripts/facelock /usr/bin/facelock

# ── Systemd service ───────────────────────────────────────────
# v3: install the repo's service file directly — no echo-write here.
# The CMake install step already copies it, but we also do it manually
# so the installer works without a cmake --install.
info "Installing systemd service (facelockd)"
install -m 644 systemd/facelockd.service /etc/systemd/system/facelockd.service

systemctl daemon-reload
systemctl enable facelockd
systemctl restart facelockd

# ── Wait for daemon ready ─────────────────────────────────────
info "Waiting for daemon ready signal ..."
READY=0
for i in $(seq 1 60); do
  # Prefer the ready file (written by daemon after ONNX+camera warmup)
  if [ -f /run/facelock/ready ]; then
    READY=1
    break
  fi
  # Fallback: socket appeared (daemon at least bound the socket)
  if [ -S /run/facelock/facelock.sock ]; then
    READY=1
    break
  fi
  sleep 0.5
done

if [ "$READY" -eq 0 ]; then
  warn "Daemon did not become ready within 30s"
  systemctl status facelockd --no-pager || true
  die "Installation aborted — check 'journalctl -u facelockd -n 50'"
fi
ok "Daemon is ready"

# ── PAM integration ───────────────────────────────────────────
info "Configuring PAM"

pam_insert /etc/pam.d/sudo
pam_insert /etc/pam.d/login

DM_FOUND=0
if systemctl is-active --quiet lightdm 2>/dev/null || [ -f /etc/pam.d/lightdm ]; then
  pam_insert /etc/pam.d/lightdm
  pam_insert /etc/pam.d/lightdm-greeter 2>/dev/null || true
  DM_FOUND=1
  ok "LightDM configured"
fi

if systemctl is-active --quiet gdm3 2>/dev/null || [ -f /etc/pam.d/gdm-password ]; then
  pam_insert /etc/pam.d/gdm-password
  DM_FOUND=1
  ok "GDM3 configured"
fi

if systemctl is-active --quiet sddm 2>/dev/null || [ -f /etc/pam.d/sddm ]; then
  pam_insert /etc/pam.d/sddm
  DM_FOUND=1
  ok "SDDM configured"
fi

if [ "$DM_FOUND" -eq 0 ]; then
  warn "No display manager detected. Add face auth manually:"
  warn "  sudo sed -i '1s/^/auth sufficient pam_facelock.so\\n/' /etc/pam.d/<your-dm>"
fi

# PAM test service
tee /etc/pam.d/facelock-test >/dev/null <<'PAMEOF'
auth sufficient pam_facelock.so
PAMEOF

# ── Enrollment ───────────────────────────────────────────────
echo ""
echo "──────────────────────────────────────────────────────────────"
echo "  AstraLock v3 is ready to enroll your face."
echo "  Ensure your camera is unobstructed and well-lit."
echo "  Liveness detection is enabled — please blink naturally."
echo "──────────────────────────────────────────────────────────────"
read -r -p "  Enroll face for '$USER_NAME' now? [Y/n] " _enroll_reply
_enroll_reply="${_enroll_reply:-Y}"

if [[ "$_enroll_reply" =~ ^[Yy]$ ]]; then
  info "Starting enrollment — look at the camera ..."
  facelock enroll "$USER_NAME"

  info "Testing PAM (running as $USER_NAME)"
  if su -s /bin/sh "$USER_NAME" -c \
      "pamtester facelock-test '${USER_NAME}' authenticate"; then
    ok "PAM face auth test passed"
  else
    warn "PAM test did not pass — non-fatal. Test manually:"
    warn "  facelock verify $USER_NAME"
  fi
else
  info "Enrollment skipped. When ready:"
  info "  facelock enroll $USER_NAME"
fi

# ── Done ──────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║         AstraLock v3 installation complete                       ║"
echo "╠══════════════════════════════════════════════════════════════════╣"
echo "║  Face auth active for: sudo, login, lock screen                  ║"
echo "║  Config:   /etc/facelock/facelock.conf                           ║"
echo "║    → Set CAMERA_DEVICE=N for IR camera  (ls /dev/video*)         ║"
echo "║    → Restart after changes: systemctl restart facelockd          ║"
echo "║                                                                  ║"
echo "║  CLI commands:                                                   ║"
echo "║    facelock enroll <username>   Enroll face for a user           ║"
echo "║    facelock verify <username>   Run a face authentication check  ║"
echo "║    facelock test   <username>   Run PAM stack test via pamtester ║"
echo "║    facelock delete <username>   Remove a user's face data        ║"
echo "║    facelock list                List enrolled users              ║"
echo "║    facelock status              Daemon health, version           ║"
echo "║    facelock ping                Raw daemon ping (JSON)           ║"
echo "║    facelock -h                  Shows all the commands           ║"
echo "╚══════════════════════════════════════════════════════════════════╝"
