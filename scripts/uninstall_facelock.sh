#!/usr/bin/env bash
set -e

echo "[*] Stopping service"
systemctl stop facelock || true
systemctl disable facelock || true

echo "[*] Removing files"
rm -f /etc/systemd/system/facelock.service
rm -f /etc/pam.d/facelock-test
rm -f /lib/x86_64-linux-gnu/security/pam_facelock.so
rm -f /usr/bin/facelock
rm -rf /usr/lib/facelock
rm -rf /usr/share/facelock
rm -rf /var/lib/facelock
rm -rf /run/facelock

systemctl daemon-reexec
systemctl daemon-reload

echo "[✓] Facelock fully removed"
