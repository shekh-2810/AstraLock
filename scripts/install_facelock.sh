#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════════════════╗
# ║              AstraLock v3.1  —  Universal Installer                      ║
# ║  Debian · Ubuntu · Kali · Arch · Fedora · RHEL · openSUSE                ║
# ╚══════════════════════════════════════════════════════════════════════════╝
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail
IFS=$'\n\t'

# ── Version & release config ──────────────────────────────────────────────────
ASTRALOCK_VERSION="3.0"
RELEASE_BASE="https://github.com/shekh-2810/AstraLock/releases/download/v${ASTRALOCK_VERSION}"
ONNX_VER="1.17.3"
REPO_URL="https://github.com/shekh-2810/AstraLock.git"

# ── Constants ─────────────────────────────────────────────────────────────────
MODEL_DIR="/usr/share/facelock/models"
MODEL_PATH="$MODEL_DIR/w600k_mbf.onnx"
DETECTOR_PATH="$MODEL_DIR/retinaface.onnx"
CONFIG_DEST="/etc/facelock/facelock.conf"

# ── Colours ───────────────────────────────────────────────────────────────────
if [ -t 1 ]; then
  BOLD='\033[1m'; DIM='\033[2m'; RED='\033[0;31m'
  GRN='\033[0;32m'; YLW='\033[0;33m'; CYN='\033[0;36m'; RST='\033[0m'
else
  BOLD=''; DIM=''; RED=''; GRN=''; YLW=''; CYN=''; RST=''
fi

# ── Logging ───────────────────────────────────────────────────────────────────
info()  { echo -e "  ${CYN}[*]${RST} $*"; }
ok()    { echo -e "  ${GRN}[✓]${RST} $*"; }
warn()  { echo -e "  ${YLW}[!]${RST} $*"; }
die()   { echo -e "  ${RED}[✗]${RST} $*" >&2; exit 1; }
step()  { echo -e "\n${BOLD}${CYN}▶ $*${RST}"; }
hr()    { echo -e "${DIM}──────────────────────────────────────────────────────────${RST}"; }

# ── Banner ────────────────────────────────────────────────────────────────────
banner() {
  echo -e "${CYN}"
  cat << 'BANNER'
   ___         __           __              __  
  / _ | ___ / /________ _ / /  ___  ____  / /__
 / __ |(_-</ __/ __/ _ `// /  / _ \/ __/ /  '_/
/_/ |_/___/\__/_/  \_,_//_/   \___/\__/ /_/\_\ 

BANNER
  echo -e "${RST}  ${BOLD}AstraLock v${ASTRALOCK_VERSION}${RST} — Face-based Linux Authentication"
  hr
}

# ── Root check ────────────────────────────────────────────────────────────────
check_root() {
  if [ "$(id -u)" -ne 0 ]; then
    die "This installer must be run as root.\n     Re-run: sudo bash $0 ${USER_NAME:-<username>}"
  fi
}

# ── Argument handling ─────────────────────────────────────────────────────────
parse_args() {
  USER_NAME="${1:-}"
  if [ -z "$USER_NAME" ]; then
    warn "No username provided."
    read -r -p "  Enter the username to enroll: " USER_NAME
    [ -z "$USER_NAME" ] && die "Username cannot be empty."
  fi
  if ! id "$USER_NAME" &>/dev/null; then
    die "User '$USER_NAME' does not exist on this system."
  fi
  ok "Target user: ${BOLD}$USER_NAME${RST}"
}

# ── Distro detection ──────────────────────────────────────────────────────────
detect_distro() {
  step "Detecting distribution"

  OS_ID=""; OS_LIKE=""; PKG_MGR=""; DISTRO_FAMILY=""

  if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS_ID="${ID:-}"
    OS_LIKE="${ID_LIKE:-}"
  fi

  case "$OS_ID" in
    ubuntu|debian|kali|linuxmint|pop|elementary|raspbian)
      PKG_MGR="apt";    DISTRO_FAMILY="debian" ;;
    arch|manjaro|endeavouros|garuda)
      PKG_MGR="pacman"; DISTRO_FAMILY="arch" ;;
    fedora)
      PKG_MGR="dnf";    DISTRO_FAMILY="fedora" ;;
    rhel|centos|rocky|almalinux|ol)
      PKG_MGR="dnf";    DISTRO_FAMILY="rhel" ;;
    opensuse*|sles)
      PKG_MGR="zypper"; DISTRO_FAMILY="suse" ;;
    *)
      case "$OS_LIKE" in
        *debian*|*ubuntu*) PKG_MGR="apt";    DISTRO_FAMILY="debian" ;;
        *arch*)            PKG_MGR="pacman"; DISTRO_FAMILY="arch"   ;;
        *fedora*|*rhel*)   PKG_MGR="dnf";   DISTRO_FAMILY="fedora" ;;
        *suse*)            PKG_MGR="zypper"; DISTRO_FAMILY="suse"   ;;
        *)
          if   command -v apt-get &>/dev/null; then PKG_MGR="apt";    DISTRO_FAMILY="debian"
          elif command -v pacman  &>/dev/null; then PKG_MGR="pacman"; DISTRO_FAMILY="arch"
          elif command -v dnf     &>/dev/null; then PKG_MGR="dnf";    DISTRO_FAMILY="fedora"
          elif command -v zypper  &>/dev/null; then PKG_MGR="zypper"; DISTRO_FAMILY="suse"
          else die "Could not detect a supported package manager (apt/pacman/dnf/zypper)."
          fi ;;
      esac ;;
  esac

  ok "Distro: ${BOLD}${PRETTY_NAME:-$OS_ID}${RST}  |  Package manager: ${BOLD}$PKG_MGR${RST}  |  Family: ${BOLD}$DISTRO_FAMILY${RST}"
}

# ── PAM .so install path ──────────────────────────────────────────────────────
set_pam_path() {
  if   [ -d /lib/x86_64-linux-gnu/security ];  then PAM_SO_DIR="/lib/x86_64-linux-gnu/security"
  elif [ -d /lib/aarch64-linux-gnu/security ]; then PAM_SO_DIR="/lib/aarch64-linux-gnu/security"
  elif [ -d /lib/security ];                   then PAM_SO_DIR="/lib/security"
  elif [ -d /usr/lib/security ];               then PAM_SO_DIR="/usr/lib/security"
  elif [ -d /usr/lib64/security ];             then PAM_SO_DIR="/usr/lib64/security"
  else
    PAM_SO_DIR="$(dirname "$(find /lib /usr/lib /usr/lib64 -name pam_unix.so 2>/dev/null | head -1)")"
    [ -d "$PAM_SO_DIR" ] || die "Cannot locate PAM security module directory."
  fi
  info "PAM module directory: $PAM_SO_DIR"
}

# ── Install build dependencies ────────────────────────────────────────────────
install_deps() {
  step "Installing build dependencies"

  case "$PKG_MGR" in
    apt)
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq
      apt-get install -y \
        git cmake ninja-build g++ \
        libpam0g-dev libaudit-dev \
        libopencv-dev libspdlog-dev \
        nlohmann-json3-dev libsystemd-dev \
        pkg-config pamtester jq wget curl
      ;;

    pacman)
      pacman -Sy --noconfirm \
        git cmake ninja gcc \
        pam audit \
        opencv spdlog \
        nlohmann-json systemd \
        pkgconf wget curl jq
      if ! command -v pamtester &>/dev/null; then
        warn "pamtester not available via pacman — PAM test step will be skipped"
        SKIP_PAMTEST=1
      fi
      ;;

    dnf)
      # Enable EPEL/CodeReady on RHEL-family for extra packages
      if [ "$DISTRO_FAMILY" = "rhel" ]; then
        dnf install -y epel-release 2>/dev/null || true
        dnf config-manager --set-enabled crb 2>/dev/null || true
      fi
      # NOTE: nlohmann-json is NOT packaged for Fedora/RHEL.
      # It is header-only; CMakeLists.txt handles it via FetchContent fallback.
      dnf install -y \
        git cmake ninja-build gcc-c++ \
        pam-devel audit-libs-devel \
        opencv-devel spdlog-devel \
        systemd-devel \
        pkgconfig wget curl jq
      if ! command -v pamtester &>/dev/null; then
        warn "pamtester not in DNF repos — PAM test step will be skipped"
        SKIP_PAMTEST=1
      fi
      ;;

    zypper)
      zypper --non-interactive install \
        git cmake ninja gcc-c++ \
        pam-devel audit-devel \
        opencv-devel spdlog-devel \
        nlohmann_json-devel systemd-devel \
        pkg-config wget curl jq
      if ! command -v pamtester &>/dev/null; then
        warn "pamtester not available via zypper — PAM test step will be skipped"
        SKIP_PAMTEST=1
      fi
      ;;
  esac

  ok "Dependencies installed"
}

# ── ONNX Runtime ──────────────────────────────────────────────────────────────
install_onnx() {
  step "Setting up ONNX Runtime"

  ONNX_INSTALLED=0

  # 1. Try distro package first
  case "$PKG_MGR" in
    apt)
      if apt-cache show libonnxruntime-dev &>/dev/null 2>&1; then
        apt-get install -y libonnxruntime-dev && ONNX_INSTALLED=1 && ok "ONNX Runtime via apt"
      fi
      ;;
    pacman)
      if pacman -Si onnxruntime &>/dev/null 2>&1; then
        pacman -S --noconfirm onnxruntime && ONNX_INSTALLED=1 && ok "ONNX Runtime via pacman"
      fi
      ;;
    dnf)
      if dnf info onnxruntime-devel &>/dev/null 2>&1; then
        dnf install -y onnxruntime-devel && ONNX_INSTALLED=1 && ok "ONNX Runtime via dnf"
      fi
      ;;
  esac

  # 2. Fallback: upstream tgz (works on all distros)
  if [ "$ONNX_INSTALLED" -eq 0 ]; then
    info "Distro package not available — downloading upstream ONNX Runtime v${ONNX_VER}"
    ARCH="$(uname -m)"
    case "$ARCH" in
      x86_64)  ONNX_ARCH="x64" ;;
      aarch64) ONNX_ARCH="aarch64" ;;
      *)        die "Unsupported CPU architecture: $ARCH (only x86_64 and aarch64 supported)" ;;
    esac

    ONNX_TGZ="/tmp/onnxruntime-${ONNX_VER}.tgz"
    ONNX_URL="https://github.com/microsoft/onnxruntime/releases/download/v${ONNX_VER}/onnxruntime-linux-${ONNX_ARCH}-${ONNX_VER}.tgz"
    wget -q --show-progress "$ONNX_URL" -O "$ONNX_TGZ" \
      || die "Failed to download ONNX Runtime. Check network."
    tar -xzf "$ONNX_TGZ" -C /usr/local --strip-components=1
    ldconfig
    rm -f "$ONNX_TGZ"
    ok "ONNX Runtime ${ONNX_VER} installed to /usr/local"
  fi
}

# ── Source acquisition ────────────────────────────────────────────────────────
# When run via curl | bash, there is no repo on disk — clone it.
# When run from inside the repo, stay put.
ensure_source() {
  if [ -f "CMakeLists.txt" ] && [ -f "pam/CMakeLists.txt" ]; then
    REPO_DIR="$(pwd)"
    info "Running from existing repo: $REPO_DIR"
    return
  fi

  step "Cloning AstraLock source"
  REPO_DIR="/tmp/astralock-src"
  if [ -d "$REPO_DIR/.git" ]; then
    info "Source already cloned — pulling latest"
    git -C "$REPO_DIR" pull -q
  else
    git clone --depth=1 --branch "v${ASTRALOCK_VERSION}" "$REPO_URL" "$REPO_DIR" \
      || git clone --depth=1 "$REPO_URL" "$REPO_DIR" \
      || die "Could not clone AstraLock repo. Check network."
  fi
  cd "$REPO_DIR"
  REPO_DIR="$(pwd)"
  ok "Source ready at $REPO_DIR"
}

# ── Download ONNX models ──────────────────────────────────────────────────────
download_models() {
  step "Downloading face models"
  mkdir -p "$MODEL_DIR"

  if [ ! -f "$MODEL_PATH" ]; then
    info "Fetching ArcFace model (w600k_mbf.onnx) ..."
    wget -q --show-progress "${RELEASE_BASE}/w600k_mbf.onnx" -O "$MODEL_PATH" \
      || die "ArcFace model download failed. Check: $RELEASE_BASE"
    ok "ArcFace model ready"
  else
    info "ArcFace model already cached — skipping"
  fi

  if [ ! -f "$DETECTOR_PATH" ]; then
    info "Fetching RetinaFace detector model ..."
    wget -q --show-progress "${RELEASE_BASE}/retinaface.onnx" -O "$DETECTOR_PATH" \
      || die "RetinaFace model download failed."
    ok "RetinaFace model ready"
  else
    info "RetinaFace model already cached — skipping"
  fi
}

# ── Write config ──────────────────────────────────────────────────────────────
write_config() {
  step "Writing config"
  mkdir -p /var/lib/facelock /etc/facelock

  if [ ! -f "$CONFIG_DEST" ]; then
    if [ -f "packaging/config/facelock.conf" ]; then
      install -m 644 "packaging/config/facelock.conf" "$CONFIG_DEST"
    else
      cat > "$CONFIG_DEST" << 'CONF'
# AstraLock v3 — /etc/facelock/facelock.conf
# Uncomment and edit as needed. Restart after changes:
#   systemctl restart facelockd

# SOCKET_PATH=/run/facelock/facelock.sock
# DATA_DIR=/var/lib/facelock/
# ONNX_MODEL_PATH=/usr/share/facelock/models/w600k_mbf.onnx
# DETECTOR_MODEL_PATH=/usr/share/facelock/models/retinaface.onnx
# CAMERA_DEVICE=0        # run: ls /dev/video* to find yours
# ONNX_THRESHOLD=0.30    # global fallback (per-user overrides at enrollment)
# ENROLL_TARGET=20
# ENROLL_MIN=10
# ENROLL_THRESHOLD_K=1.5
# LIVENESS_ENABLED=true
# LIVENESS_TEXTURE_MIN=0.15
# LIVENESS_BLINK_FRAMES=12
# LIVENESS_EAR_THRESHOLD=0.22
# IPC_THREADS=4
# CAMERA_WARMUP_FRAMES=5
CONF
    fi
    ok "Config installed to $CONFIG_DEST"
    info "  → Set CAMERA_DEVICE=N for your camera  (ls /dev/video*)"
    info "  → Restart after edits: systemctl restart facelockd"
  else
    info "Config already exists — not overwriting ($CONFIG_DEST)"
  fi
}

# ── AppArmor ──────────────────────────────────────────────────────────────────
setup_apparmor() {
  if command -v apparmor_parser &>/dev/null && [ -d "packaging/apparmor" ]; then
    step "Installing AppArmor drop-ins"
    mkdir -p /etc/apparmor.d/local
    for profile in packaging/apparmor/*; do
      [ -f "$profile" ] || continue
      install -m 644 "$profile" /etc/apparmor.d/local/
      base="/etc/apparmor.d/$(basename "$profile")"
      [ -f "$base" ] && apparmor_parser -r "$base" 2>/dev/null || true
    done
    ok "AppArmor drop-ins installed"
  fi
}

# ── Build ─────────────────────────────────────────────────────────────────────
build_and_install() {
  step "Building AstraLock (daemon + PAM module)"

  CMAKE_GEN=""
  command -v ninja &>/dev/null && CMAKE_GEN="-G Ninja"

  rm -rf build build-pam

  info "Building daemon ..."
  cmake -S . -B build $CMAKE_GEN \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
        -Wno-dev
  cmake --build build -j"$(nproc)"
  cmake --install build
  ldconfig
  ok "Daemon built and installed"

  info "Building PAM module ..."
  cmake -S pam -B build-pam $CMAKE_GEN \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -Wno-dev
  cmake --build build-pam -j"$(nproc)"
  install -m 755 build-pam/pam_facelock.so "$PAM_SO_DIR/"
  ok "pam_facelock.so → $PAM_SO_DIR/"

  info "Installing CLI ..."
  install -m 755 scripts/facelock /usr/bin/facelock
  ok "facelock CLI → /usr/bin/facelock"
}

# ── Systemd service ───────────────────────────────────────────────────────────
setup_service() {
  step "Enabling facelockd service"
  install -m 644 systemd/facelockd.service /etc/systemd/system/facelockd.service
  systemctl daemon-reload
  systemctl enable --now facelockd

  info "Waiting for daemon to become ready ..."
  for i in $(seq 1 60); do
    [ -f /run/facelock/ready ] && break
    [ -S /run/facelock/facelock.sock ] && break
    sleep 0.5
  done

  if [ ! -f /run/facelock/ready ] && [ ! -S /run/facelock/facelock.sock ]; then
    warn "Daemon did not signal ready within 30 s."
    systemctl status facelockd --no-pager || true
    die "Check logs: journalctl -u facelockd -n 50"
  fi
  ok "Daemon is ready"
}

# ── PAM configuration ─────────────────────────────────────────────────────────
pam_insert() {
  local svc="$1"
  local rule="auth sufficient pam_facelock.so"
  [ -f "$svc" ] || { warn "$svc not found — skipping"; return; }
  if grep -qF "pam_facelock.so" "$svc"; then
    info "PAM rule already present in $svc"
  else
    sed -i "1s|^|${rule}\n|" "$svc"
    ok "PAM rule added → $svc"
  fi
}

setup_pam() {
  step "Configuring PAM"

  pam_insert /etc/pam.d/sudo
  pam_insert /etc/pam.d/login

  DM_FOUND=0
  for dm_check in \
    "lightdm:/etc/pam.d/lightdm" \
    "lightdm:/etc/pam.d/lightdm-greeter" \
    "gdm:/etc/pam.d/gdm-password" \
    "gdm3:/etc/pam.d/gdm-password" \
    "sddm:/etc/pam.d/sddm" \
    "lxdm:/etc/pam.d/lxdm" \
    "xdm:/etc/pam.d/xdm" \
    "slim:/etc/pam.d/slim"; do
    dm="${dm_check%%:*}"
    pam_file="${dm_check##*:}"
    if systemctl is-active --quiet "$dm" 2>/dev/null || [ -f "$pam_file" ]; then
      pam_insert "$pam_file"
      DM_FOUND=1
    fi
  done

  if [ "$DM_FOUND" -eq 0 ]; then
    warn "No display manager detected. To add face auth to your DM:"
    warn "  sudo sed -i '1s/^/auth sufficient pam_facelock.so\\n/' /etc/pam.d/<your-dm>"
  fi

  tee /etc/pam.d/facelock-test > /dev/null << 'PAMEOF'
auth sufficient pam_facelock.so
PAMEOF
  ok "PAM configuration complete"
}

# ── Enrollment ────────────────────────────────────────────────────────────────
enroll_user() {
  step "Face enrollment"
  echo ""
  hr
  echo -e "  ${BOLD}AstraLock is ready to enroll: ${CYN}$USER_NAME${RST}"
  echo    "  → Make sure your camera is unobstructed and well-lit."
  echo    "  → Liveness detection is active — blink naturally."
  hr
  echo ""

  read -r -p "  Enroll face for '$USER_NAME' now? [Y/n] " _reply
  _reply="${_reply:-Y}"

  if [[ "$_reply" =~ ^[Yy]$ ]]; then
    info "Starting enrollment — look at the camera ..."
    facelock enroll "$USER_NAME"

    if [ "${SKIP_PAMTEST:-0}" -eq 0 ]; then
      info "Running PAM integration test ..."
      if su -s /bin/sh "$USER_NAME" -c \
          "pamtester facelock-test '${USER_NAME}' authenticate" 2>/dev/null; then
        ok "PAM face auth test passed!"
      else
        warn "PAM test did not pass — non-fatal. Verify manually:"
        warn "  facelock verify $USER_NAME"
      fi
    fi
  else
    info "Skipped. Enroll when ready:"
    info "  facelock enroll $USER_NAME"
  fi
}

# ── Done banner ───────────────────────────────────────────────────────────────
done_banner() {
  echo ""
  echo -e "${GRN}${BOLD}"
  cat << 'DONE'
╔══════════════════════════════════════════════════════════════════╗
║         AstraLock v3.1  —  Installation Complete  🔓             ║
╠══════════════════════════════════════════════════════════════════╣
║  Face auth active for: sudo · login · lock screen                ║
║                                                                  ║
║  Config:  /etc/facelock/facelock.conf                            ║
║    → Set CAMERA_DEVICE=N for IR camera  (ls /dev/video*)         ║
║    → Restart after edits: systemctl restart facelockd            ║ 
║                                                                  ║
║  CLI commands:                                                   ║
║    facelock enroll <user>   — Enroll face                        ║
║    facelock verify <user>   — Direct face check                  ║
║    facelock test   <user>   — Full PAM stack test                ║
║    facelock list            — Show enrolled users                ║
║    facelock delete <user>   — Remove face data                   ║
║    facelock status          — Daemon health & version            ║
║    facelock doctor          — System health check                ║
║    facelock -h              — Show all commands                  ║
╚══════════════════════════════════════════════════════════════════╝
DONE
  echo -e "${RST}"
}

# ══════════════════════════════════════════════════════════════════════════════
#  MAIN
# ══════════════════════════════════════════════════════════════════════════════
SKIP_PAMTEST=0

banner
check_root
parse_args "$@"
detect_distro
set_pam_path
install_deps
install_onnx
ensure_source
download_models
write_config
setup_apparmor
build_and_install
setup_service
setup_pam
enroll_user
done_banner
