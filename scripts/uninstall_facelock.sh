#!/usr/bin/env bash
set -e

echo "[*] Stopping service"
systemctl stop facelock || true
systemctl disable facelock || true

echo "[*] Removing PAM rules"
for svc in /etc/pam.d/sudo /etc/pam.d/login \
           /etc/pam.d/lightdm /etc/pam.d/lightdm-greeter \
           /etc/pam.d/gdm-password /etc/pam.d/sddm; do
  if [ -f "$svc" ] && grep -qF "pam_facelock.so" "$svc"; then
    sed -i '/pam_facelock\.so/d' "$svc"
    echo "[*] Removed facelock rule from $svc"
  fi
done

echo "[*] Removing files"
rm -f /etc/systemd/system/facelock.service
rm -f /etc/pam.d/facelock-test
rm -f /lib/x86_64-linux-gnu/security/pam_facelock.so
rm -f /usr/bin/facelock
rm -rf /usr/lib/facelock
rm -rf /usr/share/facelock
rm -rf /var/lib/facelock
rm -rf /run/facelock
rm -f /etc/facelock/facelock.conf
rmdir /etc/facelock 2>/dev/null || true

echo "[*] Removing AppArmor drop-in"
rm -f /etc/apparmor.d/local/usr.bin.nc.openbsd
if command -v apparmor_parser >/dev/null 2>&1; then
  if [ -f /etc/apparmor.d/usr.bin.nc.openbsd ]; then
    apparmor_parser -r /etc/apparmor.d/usr.bin.nc.openbsd 2>/dev/null || true
  fi
fi

systemctl daemon-reexec
systemctl daemon-reload

echo "[✓] AstraLock fully removed"
