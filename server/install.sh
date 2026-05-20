#!/usr/bin/env bash
# raa-server installer
# Usage:
#   bash install.sh          # installs latest release
#   bash install.sh v1.2.3   # installs specific version
set -euo pipefail

REPO="daig0rian/remote-audio-aggregation"
INSTALL_DIR="${INSTALL_DIR:-/opt/raa-server}"

# ── Resolve version ────────────────────────────────────────────────────────────
if [[ "${1:-}" =~ ^v?[0-9] ]]; then
    VERSION="${1#v}"
else
    echo "==> Fetching latest release version..."
    VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
        | grep '"tag_name"' | sed 's/.*"v\([^"]*\)".*/\1/')
fi
echo "==> raa-server v${VERSION} → ${INSTALL_DIR}"

# ── Prerequisite check ─────────────────────────────────────────────────────────
for cmd in node npm curl tar; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "ERROR: '$cmd' not found. Aborting."; exit 1; }
done

NODE_MAJOR=$(node -e 'process.stdout.write(process.versions.node.split(".")[0])')
if [[ "$NODE_MAJOR" -lt 24 ]]; then
    echo "ERROR: Node.js >= 24 required (found v${NODE_MAJOR}). Install via nvm:"
    echo "  nvm install 24 && nvm use 24"
    exit 1
fi

# @evan/opus needs these to compile its native addon
if ! dpkg -s build-essential libopus-dev >/dev/null 2>&1; then
    echo "==> Installing build-essential and libopus-dev..."
    sudo apt-get update -qq
    sudo apt-get install -y build-essential libopus-dev
fi

# ── Download ───────────────────────────────────────────────────────────────────
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

TARBALL_URL="https://github.com/${REPO}/releases/download/v${VERSION}/raa-server-${VERSION}.tgz"
echo "==> Downloading ${TARBALL_URL} ..."
curl -fsSL "$TARBALL_URL" -o "$TMP/raa-server.tgz"

# ── Extract ────────────────────────────────────────────────────────────────────
echo "==> Extracting to ${INSTALL_DIR} ..."
sudo mkdir -p "$INSTALL_DIR"
# npm pack puts files under package/ — strip that prefix
sudo tar -xzf "$TMP/raa-server.tgz" -C "$INSTALL_DIR" --strip-components=1

# ── npm install (builds @evan/opus native addon) ───────────────────────────────
echo "==> Running npm install (building @evan/opus — may take a minute)..."
cd "$INSTALL_DIR"
sudo npm install --omit=dev

# ── Done ───────────────────────────────────────────────────────────────────────
echo ""
echo "Installation complete."
echo ""
echo "Start the server:"
echo "  node ${INSTALL_DIR}/src/main.js"
echo ""
echo "Or set up systemd (see README for the service file)."
