#!/usr/bin/env bash
# ============================================================
#  AstraLock v3 — uninstaller
# ============================================================
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "[!] This script must be run as root (sudo $0)"
  exit 1
fi

echo "[*] Stopping facelockd service"
systemctl stop facelockd    2>/dev/null || true
systemctl disable facelockd 2>/dev/null || true

echo "[*] Removing PAM rules"
for svc in /etc/pam.d/sudo \
           /etc/pam.d/login \
           /etc/pam.d/lightdm \
           /etc/pam.d/lightdm-greeter \
           /etc/pam.d/gdm-password \
           /etc/pam.d/sddm; do
  if [ -f "$svc" ] && grep -qF "pam_facelock.so" "$svc"; then
    sed -i '/pam_facelock\.so/d' "$svc"
    echo "[*] Removed facelock rule from $svc"
  fi
done

echo "[*] Removing installed files"
rm -f  /etc/systemd/system/facelockd.service
rm -f  /etc/pam.d/facelock-test
rm -f  /lib/x86_64-linux-gnu/security/pam_facelock.so
rm -f  /usr/bin/facelock
rm -f  /usr/sbin/facelockd
rm -rf /usr/share/facelock
rm -f  /run/facelock/ready
rm -f  /run/facelock/facelock.sock
rmdir  /run/facelock 2>/dev/null || true

echo "[*] Removing AppArmor drop-in"
rm -f /etc/apparmor.d/local/usr.bin.nc.openbsd
if command -v apparmor_parser >/dev/null 2>&1; then
  if [ -f /etc/apparmor.d/usr.bin.nc.openbsd ]; then
    apparmor_parser -r /etc/apparmor.d/usr.bin.nc.openbsd 2>/dev/null || true
  fi
fi

# Ask before wiping face data — this is irreversible
echo ""
echo "[!] /var/lib/facelock/ contains all enrolled face embeddings."
read -r -p "    Delete face data for all users? [y/N] " _confirm
if [[ "$_confirm" =~ ^[Yy]$ ]]; then
  rm -rf /var/lib/facelock
  echo "[*] Face data removed"
else
  echo "[*] Face data kept at /var/lib/facelock/"
  echo "    Remove manually when ready: sudo rm -rf /var/lib/facelock"
fi

# Ask before removing config
if [ -f /etc/facelock/facelock.conf ]; then
  read -r -p "    Delete /etc/facelock/facelock.conf? [y/N] " _conf
  if [[ "$_conf" =~ ^[Yy]$ ]]; then
    rm -f /etc/facelock/facelock.conf
    rmdir /etc/facelock 2>/dev/null || true
    echo "[*] Config removed"
  else
    echo "[*] Config kept at /etc/facelock/facelock.conf"
  fi
fi

systemctl daemon-reload

echo ""
echo "[✓] AstraLock v3 fully removed"