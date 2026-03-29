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
mkdir -p /usr/lib/facelock /var/lib/facelock /usr/share/facelock

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
Description=Facelock biometric authentication daemon
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

echo "[*] Creating PAM test service"
tee /etc/pam.d/facelock-test >/dev/null <<EOF
auth sufficient pam_facelock.so
EOF

echo "[*] Initial enrollment"
facelock enroll "$USER_NAME"

echo "[*] Testing PAM"
pamtester facelock-test "$USER_NAME" authenticate || \
  su - "$USER_NAME" -c "pamtester facelock-test $USER_NAME authenticate"

echo "[✓] AstraLock v2.0 installation complete"
