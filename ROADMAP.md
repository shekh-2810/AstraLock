# AstraLock Roadmap

This document outlines the planned evolution of AstraLock.
Dates are intentionally omitted — features ship when stable.

---

## v0.1 (older — unstable)

**Scope**
- Debian-based systems only (Debian, Ubuntu, Kali)
- LBPH-based face recognition
- PAM integration for login / sudo / polkit
- systemd-managed daemon
- netcat socket IPC

**Status**
- Released
- Tagged as `v0.1.0`

---

## v0.2 (current release)

**Goals**
- Stabilize installer on Debian-based systems
- Clarify supported environments (Debian-only)
- Improve IPC robustness documentation
- Better CLI UX for `enroll`, `verify`, `test`
- Dependency detection and clearer failure modes

**Non-Goals**
- No ONNX
- No new distro support
- No protocol changes

---

## Design Philosophy

- Local-first
- Offline by default
- No cloud dependency
- Transparent and auditable
- PAM remains authoritative

