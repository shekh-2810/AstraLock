#!/usr/bin/env bash
set -e

USER_NAME="$1"

if [ -z "$USER_NAME" ]; then
  echo "Usage: sudo $0 <username>"
  exit 1
fi

ONNX_VER="1.17.3"
MODEL_DIR="/usr/share/facelock/models"
MODEL_PATH="$MODEL_DIR/w600k_mbf.onnx"
DETECTOR_PATH="$MODEL_DIR/retinaface.onnx"
MODEL_URL="https://github.com/shekh-2810/AstraLock/releases/download/v2.0/w600k_mbf.onnx"
DETECTOR_URL="https://github.com/shekh-2810/AstraLock/releases/download/v2.0/retinaface.onnx"

# ── helper: insert a PAM rule at the top of a service file if not already there ──
pam_insert() {
  local svc="$1"  # e.g. /etc/pam.d/sudo
  local rule="auth sufficient pam_facelock.so"
  if [ -f "$svc" ] && ! grep -qF "pam_facelock.so" "$svc"; then
    sed -i "1s|^|${rule}\n|" "$svc"
    echo "[*] PAM rule added to $svc"
  elif [ ! -f "$svc" ]; then
    echo "[!] $svc not found — skipping"
  else
    echo "[*] PAM rule already present in $svc — skipping"
  fi
}

echo "[*] Installing dependencies"
apt update
apt install -y \
  cmake ninja-build g++ \
  libpam0g-dev libaudit-dev \
  libopencv-dev \
  libspdlog-dev \
  nlohmann-json3-dev \
  pkg-config \
  python3 python3-opencv \
  netcat-openbsd \
  pamtester jq wget

echo "[*] Installing ONNX Runtime"
if ! apt install -y libonnxruntime-dev 2>/dev/null; then
  echo "[*] libonnxruntime-dev not in apt, installing manually..."
  wget -q "https://github.com/microsoft/onnxruntime/releases/download/v${ONNX_VER}/onnxruntime-linux-x64-${ONNX_VER}.tgz" \
    -O /tmp/onnxruntime.tgz
  tar -xzf /tmp/onnxruntime.tgz -C /usr/local --strip-components=1
  ldconfig
  rm /tmp/onnxruntime.tgz
fi

echo "[*] Downloading models"
mkdir -p "$MODEL_DIR"

if [ ! -f "$MODEL_PATH" ]; then
  echo "[*] Downloading ArcFace model (w600k_mbf.onnx)..."
  wget -q --show-progress "$MODEL_URL" -O "$MODEL_PATH"
else
  echo "[*] ArcFace model already present, skipping"
fi

if [ ! -f "$DETECTOR_PATH" ]; then
  echo "[*] Downloading face detector model (retinaface.onnx)..."
  wget -q --show-progress "$DETECTOR_URL" -O "$DETECTOR_PATH"
else
  echo "[*] Detector model already present, skipping"
fi

echo "[*] Creating directories"
mkdir -p /usr/lib/facelock /var/lib/facelock /usr/share/facelock /etc/facelock

echo "[*] Installing config file"
if [ ! -f /etc/facelock/facelock.conf ]; then
  install -m 644 packaging/rootfs/etc/facelock/facelock.conf /etc/facelock/facelock.conf
  echo "[*] Config installed to /etc/facelock/facelock.conf"
  echo "    Edit CAMERA_DEVICE= to select your IR camera  (ls /dev/video*)"
  echo "    Edit ONNX_THRESHOLD= to tune sensitivity"
else
  echo "[*] Config already exists at /etc/facelock/facelock.conf — skipping"
fi

echo "[*] Installing AppArmor drop-in for nc.openbsd"
if command -v apparmor_parser >/dev/null 2>&1; then
  mkdir -p /etc/apparmor.d/local
  install -m 644 packaging/apparmor/usr.bin.nc.openbsd \
    /etc/apparmor.d/local/usr.bin.nc.openbsd
  if [ -f /etc/apparmor.d/usr.bin.nc.openbsd ]; then
    apparmor_parser -r /etc/apparmor.d/usr.bin.nc.openbsd 2>/dev/null || true
    echo "[*] AppArmor nc.openbsd profile reloaded"
  else
    echo "[*] AppArmor drop-in installed (applies on next boot or profile load)"
  fi
else
  echo "[*] AppArmor not active — skipping nc drop-in"
fi

echo "[*] Cleaning previous builds"
rm -rf build build-pam

echo "[*] Building daemon"
cmake -S . -B build
cmake --build build -j
cmake --install build
ldconfig

echo "[*] Building PAM module"
cmake -S pam -B build-pam
cmake --build build-pam
install -m 755 build-pam/pam_facelock.so /lib/x86_64-linux-gnu/security/

echo "[*] Installing facelock CLI"
install -m 755 scripts/facelock /usr/bin/facelock

echo "[*] Installing systemd service"
tee /etc/systemd/system/facelock.service >/dev/null <<'EOF'
[Unit]
Description=AstraLock biometric authentication daemon
After=multi-user.target

[Service]
Type=simple
ExecStart=/usr/lib/facelock/facelockd
Restart=on-failure
User=root
Group=root
RuntimeDirectory=facelock
RuntimeDirectoryMode=0755
ReadWritePaths=/run/facelock /var/lib/facelock
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reexec
systemctl daemon-reload
systemctl enable facelock
systemctl restart facelock

echo "[*] Waiting for socket..."
for i in {1..40}; do
  [ -S /run/facelock/facelock.sock ] && break
  sleep 0.25
done

if [ ! -S /run/facelock/facelock.sock ]; then
  echo "[!] facelock socket not ready"
  systemctl status facelock --no-pager
  exit 1
fi

# ── PAM integration ──────────────────────────────────────────────────────────
echo "[*] Configuring PAM authentication"

echo "[*] Adding face auth to sudo"
pam_insert /etc/pam.d/sudo

echo "[*] Adding face auth to login (tty)"
pam_insert /etc/pam.d/login

# Display manager / lock screen detection
echo "[*] Detecting display manager for lock screen support"
DM_FOUND=0

if systemctl is-active --quiet lightdm 2>/dev/null || [ -f /etc/pam.d/lightdm ]; then
  pam_insert /etc/pam.d/lightdm
  # LightDM greeter uses lightdm-greeter for unlock
  pam_insert /etc/pam.d/lightdm-greeter 2>/dev/null || true
  DM_FOUND=1
  echo "[*] LightDM configured"
fi

if systemctl is-active --quiet gdm3 2>/dev/null || [ -f /etc/pam.d/gdm-password ]; then
  pam_insert /etc/pam.d/gdm-password
  DM_FOUND=1
  echo "[*] GDM3 configured"
fi

if systemctl is-active --quiet sddm 2>/dev/null || [ -f /etc/pam.d/sddm ]; then
  pam_insert /etc/pam.d/sddm
  DM_FOUND=1
  echo "[*] SDDM configured"
fi

if [ $DM_FOUND -eq 0 ]; then
  echo "[!] No known display manager detected."
  echo "    Face unlock at login screen will not work automatically."
  echo "    To add manually: sudo sed -i '1s/^/auth sufficient pam_facelock.so\\n/' /etc/pam.d/<your-dm>"
fi
# ─────────────────────────────────────────────────────────────────────────────

echo "[*] Creating PAM test service"
tee /etc/pam.d/facelock-test >/dev/null <<EOF
auth sufficient pam_facelock.so
EOF

echo ""
echo "──────────────────────────────────────────────"
echo "  AstraLock is ready to enroll your face."
echo "  Make sure your camera is unobstructed and"
echo "  you are in a well-lit area before starting."
echo "──────────────────────────────────────────────"
read -r -p "  Enroll face for '$USER_NAME' now? [Y/n] " _enroll_reply
_enroll_reply="${_enroll_reply:-Y}"   # default to Y on Enter

if [[ "$_enroll_reply" =~ ^[Yy]$ ]]; then
  echo "[*] Starting enrollment — look at the camera..."
  facelock enroll "$USER_NAME"
else
  echo "[*] Enrollment skipped."
  echo "    Run this whenever you are ready:"
  echo "      facelock enroll $USER_NAME"
fi

echo "[*] Testing PAM (running as $USER_NAME)"
# pamtester must run as the authenticating user, not root.
# Use -s /bin/sh to avoid triggering the user's login profile (pyenv etc.)
if [[ "$_enroll_reply" =~ ^[Yy]$ ]]; then
  if su -s /bin/sh "$USER_NAME" -c "pamtester facelock-test '$USER_NAME' authenticate"; then
    echo "[✓] PAM face auth test passed"
  else
    echo "[!] PAM test skipped or face not recognised — this is non-fatal."
    echo "    Daemon is running and enrolled. Test manually with:"
    echo "      facelock verify $USER_NAME"
  fi
else
  echo "[*] PAM test skipped (no enrollment yet)."
fi

echo ""
echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║         AstraLock v2.1 installation complete                  ║"
echo "╠═══════════════════════════════════════════════════════════════╣"
echo "║  Face auth active for: sudo, login, lock screen               ║"
echo "║  Config:  /etc/facelock/facelock.conf                         ║"
echo "║    → Set CAMERA_DEVICE=N for IR camera  (ls /dev/video*)      ║"
echo "║    → Restart after config changes: systemctl restart facelock ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
