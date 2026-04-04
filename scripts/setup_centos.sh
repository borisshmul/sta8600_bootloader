#!/usr/bin/env bash
# setup_centos.sh — Install all build dependencies on CentOS 7 / 8 / Stream / RHEL
#
# Run as root or with sudo:
#   sudo bash scripts/setup_centos.sh
#
# What this installs:
#   - gcc, make, openssl-devel        (host tool build)
#   - python3                          (flash.py / secure_provision.py)
#   - ARM cross-compiler              (target BL2 build)
#   - libgpiod-utils                  (optional GPIO reset in flash.py)
#   - openssl CLI                     (key generation helper)

set -euo pipefail

# ---- Detect CentOS version ------------------------------------------------
if [ -f /etc/centos-release ]; then
    VER=$(rpm -q --qf '%{VERSION}' centos-release 2>/dev/null | cut -d. -f1)
elif [ -f /etc/redhat-release ]; then
    VER=$(rpm -q --qf '%{VERSION}' redhat-release 2>/dev/null | cut -d. -f1)
else
    echo "[!] This script is intended for CentOS / RHEL only."
    exit 1
fi

echo "[setup] Detected version: ${VER}"

PKG=""
if command -v dnf &>/dev/null; then
    PKG=dnf
else
    PKG=yum
fi

echo "[setup] Package manager: ${PKG}"

# ---- EPEL (needed for libgpiod and the ARM cross-compiler on CentOS) ------
echo "[setup] Enabling EPEL ..."
if [ "${VER}" -ge 8 ]; then
    ${PKG} install -y epel-release
    # On CentOS Stream 8+ also enable CRB (CodeReady Linux Builder) for
    # some -devel packages
    if command -v dnf &>/dev/null; then
        dnf config-manager --set-enabled crb 2>/dev/null || \
        dnf config-manager --set-enabled powertools 2>/dev/null || true
    fi
else
    # CentOS 7
    ${PKG} install -y epel-release
fi

# ---- Core build tools ------------------------------------------------------
echo "[setup] Installing core build tools ..."
${PKG} install -y \
    gcc \
    make \
    openssl \
    openssl-devel \
    python3 \
    python3-pip

# ---- ARM cross-compiler ----------------------------------------------------
echo "[setup] Installing ARM cross-compiler ..."
if [ "${VER}" -ge 8 ]; then
    # CentOS 8 / Stream: arm-linux-gnu-gcc is in EPEL
    if ${PKG} info arm-linux-gnu-gcc &>/dev/null; then
        ${PKG} install -y arm-linux-gnu-gcc arm-linux-gnu-binutils
        echo "[setup] Installed arm-linux-gnu-gcc (prefix: arm-linux-gnu-)"
    else
        echo "[setup] arm-linux-gnu-gcc not found in repos — installing ARM GNU Toolchain ..."
        install_arm_toolchain
    fi
else
    # CentOS 7: arm-linux-gnueabihf not in EPEL; use ARM's official release
    install_arm_toolchain
fi

# ---- ARM GNU Toolchain installer (fallback) --------------------------------
install_arm_toolchain() {
    local TOOLCHAIN_URL="https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-linux-gnueabihf.tar.xz"
    local INSTALL_DIR="/opt/arm-gnu-toolchain"

    echo "[setup] Downloading ARM GNU Toolchain to ${INSTALL_DIR} ..."
    echo "[setup] (This is ~200 MB — may take a few minutes)"

    ${PKG} install -y wget xz tar

    mkdir -p "${INSTALL_DIR}"
    wget -q --show-progress -O /tmp/arm-toolchain.tar.xz "${TOOLCHAIN_URL}"
    tar -xf /tmp/arm-toolchain.tar.xz -C "${INSTALL_DIR}" --strip-components=1
    rm /tmp/arm-toolchain.tar.xz

    # Add to PATH system-wide
    cat > /etc/profile.d/arm-gnu-toolchain.sh <<EOF
export PATH=\$PATH:${INSTALL_DIR}/bin
EOF
    export PATH="$PATH:${INSTALL_DIR}/bin"
    echo "[setup] ARM toolchain installed to ${INSTALL_DIR}/bin"
    echo "[setup] Cross prefix to use: arm-none-linux-gnueabihf-"
    echo "[setup] Run: make CROSS=arm-none-linux-gnueabihf- in target/"
}

# ---- Optional: libgpiod for GPIO-based reset in flash.py ------------------
echo "[setup] Installing libgpiod (optional, for GPIO reset) ..."
${PKG} install -y libgpiod-utils 2>/dev/null || \
    echo "[setup] libgpiod-utils not available — GPIO reset will not work (non-critical)"

# ---- Verify -----------------------------------------------------------------
echo ""
echo "[setup] ---- Verification ----"
gcc        --version | head -1
openssl    version
python3    --version

for try in arm-linux-gnu-gcc arm-linux-gnueabihf-gcc arm-none-linux-gnueabihf-gcc; do
    if command -v "${try}" &>/dev/null; then
        echo "ARM CC: $(${try} --version | head -1)"
        break
    fi
done

echo ""
echo "[setup] Done.  If a new shell PATH was set, run:"
echo "        source /etc/profile.d/arm-gnu-toolchain.sh"
echo "        cd host && make"
echo "        cd ../target && make         # or: make CROSS=arm-none-linux-gnueabihf-"
