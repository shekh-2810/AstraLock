#!/usr/bin/env bash
set -e

USER_NAME="$1"

if [ -z "$USER_NAME" ]; then
  echo "Usage: sudo $0 <username>"
  exit 1
fi

echo "[*] Installing dependencies"
apt update
apt install -y \
  cmake ninja-build g++ \
  libpam0g-dev libaudit-dev \
  libopencv-dev pkg-config \
  python3 python3-opencv \
  socat pamtester jq

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

echo "[*] Installing helper scripts"
install -m 755 scripts/train_lbph.py /usr/share/facelock/
install -m 755 scripts/make_emb_bin_from_raw.py /usr/share/facelock/

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

[ -S /run/facelock/facelock.sock ] || {
  echo "[!] facelock socket not ready"
  systemctl status facelock --no-pager
  exit 1
}

echo "[*] Creating PAM test service"
tee /etc/pam.d/facelock-test >/dev/null <<EOF
auth sufficient pam_facelock.so
EOF

echo "[*] Initial enrollment"
facelock enroll "$USER_NAME"

echo "[*] Testing PAM"
pamtester facelock-test "$USER_NAME" authenticate

echo "[✓] Installation complete"
