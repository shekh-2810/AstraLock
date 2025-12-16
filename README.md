<p align="center">
  <img src="assets/banner.png" alt="AstraLock banner" />
</p>

# Facelock (Astralock)

**Facelock** is a Linux biometric authentication system using facial recognition,
integrated with **PAM** and **systemd**.

It provides:
- A long-running authentication daemon
- A PAM module for system login
- A CLI for enroll / verify / test
- LBPH-based local face recognition (no cloud)

---

## Features

- PAM authentication (`login`, `sudo`, `polkit`, display managers)
- Offline, local face data
- Systemd-managed daemon
- Works with webcam via OpenCV
- CLI utilities for users and admins

---

## Architecture

PAM → pam_facelock.so
  
  &#8595;

facelockd (systemd)

   ↓

UNIX socket (/run/facelock/facelock.sock)

  ↓

Face model (/var/lib/facelock)



---

## Supported Distros

| Distro        | Status |
|---------------|--------|
| Debian        | ✅ |
| Ubuntu        | ✅ |
| Kali Linux    | ✅ |
| Arch Linux    | ⚠️ (manual deps) |
| Fedora        | ⚠️ (SELinux rules needed) |

---

## Dependencies

### Debian / Ubuntu / Kali

```bash
sudo apt install -y \
  cmake ninja-build g++ \
  libpam0g-dev libaudit-dev \
  libopencv-dev \
  pkg-config \
  python3 python3-opencv \
  socat pamtester jq
```
---

## Instalation 

```bash
git clone https://github.com/<yourname>/facelock.git
cd facelock
sudo scripts/install_facelock.sh <username>
```

#### Example

```bash
sudo scripts/install_facelock.sh shekh-2810
```
This will:

- Build daemon + PAM module

- Install systemd service

- Enroll the user

- Train the model

- Verify PAM authentication
--- 

## CLI Usage
#### Enroll / Update Face
```bash
sudo facelock enroll <username>
```

Replaces old samples and retrains model.

#### Verify Face (direct)
```bash
sudo facelock verify <username>
```

Returns JSON result from daemon.


#### Test PAM
```bash
sudo facelock test <username>
```

Runs pamtester.

---

## Enabling PAM on System

**⚠️ Do this carefully. Always keep a terminal open.**

- #### Enable for sudo

Edit:
```bash
sudo nano /etc/pam.d/sudo
```

Add above @include common-auth:
```bash
auth sufficient pam_facelock.so
```

- #### Enable for login (TTY)
```bash
sudo nano /etc/pam.d/login
```

Add:
```bash
auth sufficient pam_facelock.so
```

- #### Enable for GUI (GDM / SDDM / LightDM)
  
**GDM**
```bash
sudo nano /etc/pam.d/gdm-password
```

**SDDM**
```bash
sudo nano /etc/pam.d/sddm
```

Add:
```bash
auth sufficient pam_facelock.so
```
**Polkit (GUI sudo prompts)**
```bash
sudo nano /etc/pam.d/polkit-1
```

Add:
```bash
auth sufficient pam_facelock.so
```

## Uninstall
```bash
sudo scripts/uninstall_facelock.sh
```

**Removes:**

- daemon

- PAM module

- systemd service

- user face data

### **Security Notes**

Face auth is sufficient

Password fallback remains

No biometric data leaves system

Models stored locally

### Development
**Build only**
```bash
cmake -S . -B build
cmake --build build -j
```
**Clean rebuild**
```bash
rm -rf build build-pam
```

## **Disclaimer**

This is biometric authentication software.

Always keep a fallback login method.


### License

MIT (project code)

cnpy retains its own license



