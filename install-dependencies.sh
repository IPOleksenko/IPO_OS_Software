#!/bin/bash
set -e

# ==============================================================================
# IPO_Applications - Dependency Installation Script
# ==============================================================================

echo "=========================================="
echo "Installing dependencies for IPO_Applications"
echo "=========================================="

REQUIRED_PACKAGES=(
    "build-essential"
    "gcc-multilib"
    "g++-multilib"
    "nasm"
    "python3"
    "python3-pip"
    "xfonts-intl-chinese"
    "console-setup"
)

echo "Updating package lists..."
sudo apt update || { echo "Warning: apt update failed, attempting installation anyway..."; }

for PKG in "${REQUIRED_PACKAGES[@]}"; do
    if dpkg -s "$PKG" &>/dev/null; then
        echo "✓ $PKG is already installed."
    else
        echo "→ Installing $PKG..."
        sudo apt install -y "$PKG" || echo "Warning: Failed to install $PKG"
    fi
done

echo ""
echo "Dependency installation completed."

