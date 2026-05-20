#!/usr/bin/env bash
# raa-server installer
# Usage:
#   bash install.sh          # installs latest release
#   bash install.sh v1.2.3   # installs specific version
#
# Installs to ~/raa-server and registers a systemd user service.
# The only operation requiring sudo is: apt-get (build deps) and
# loginctl enable-linger (once, so the service starts at boot without login).
set -euo pipefail

REPO="daig0rian/remote-audio-aggregation"
INSTALL_DIR="${INSTALL_DIR:-$HOME/raa-server}"
SERVICE_NAME="raa-server"
SERVICE_DIR="$HOME/.config/systemd/user"

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
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "ERROR: '$cmd' not found."
        echo "  Install Node.js >= 24 via nvm: https://github.com/nvm-sh/nvm"
        exit 1
    }
done

NODE_MAJOR=$(node -e 'process.stdout.write(process.versions.node.split(".")[0])')
if [[ "$NODE_MAJOR" -lt 24 ]]; then
    echo "ERROR: Node.js >= 24 required (found v${NODE_MAJOR})."
    echo "  nvm install 24 && nvm use 24 && nvm alias default 24"
    exit 1
fi

NODE_BIN=$(command -v node)
echo "==> Using Node.js $(node --version) at ${NODE_BIN}"

# ── Build dependencies (apt — requires sudo, skipped if already installed) ────
if ! dpkg -s build-essential libopus-dev >/dev/null 2>&1; then
    echo "==> Installing build-essential and libopus-dev (requires sudo)..."
    sudo apt-get update -qq
    sudo apt-get install -y build-essential libopus-dev
fi

# ── Stop existing service if running ──────────────────────────────────────────
if systemctl --user is-active --quiet "${SERVICE_NAME}" 2>/dev/null; then
    echo "==> Stopping existing ${SERVICE_NAME} service..."
    systemctl --user stop "${SERVICE_NAME}"
fi

# ── Download ───────────────────────────────────────────────────────────────────
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

TARBALL_URL="https://github.com/${REPO}/releases/download/v${VERSION}/raa-server-${VERSION}.tgz"
echo "==> Downloading ${TARBALL_URL} ..."
curl -fsSL "$TARBALL_URL" -o "$TMP/raa-server.tgz"

# ── Extract ────────────────────────────────────────────────────────────────────
echo "==> Installing to ${INSTALL_DIR} ..."
mkdir -p "$INSTALL_DIR"
# npm pack puts files under package/ — strip that prefix
tar -xzf "$TMP/raa-server.tgz" -C "$INSTALL_DIR" --strip-components=1

# ── npm install (builds @evan/opus native addon) ───────────────────────────────
echo "==> Running npm install (building @evan/opus — may take a minute)..."
cd "$INSTALL_DIR"
npm install --omit=dev

# ── systemd user service ───────────────────────────────────────────────────────
echo "==> Installing systemd user service..."
mkdir -p "$SERVICE_DIR"
cat > "$SERVICE_DIR/${SERVICE_NAME}.service" << EOF
[Unit]
Description=Remote Audio Aggregation Server
After=network.target

[Service]
ExecStart=${NODE_BIN} ${INSTALL_DIR}/src/main.js
WorkingDirectory=${INSTALL_DIR}
Restart=always
RestartSec=5
Environment=NODE_ENV=production
Environment=LOG_LEVEL=info
StandardOutput=journal
StandardError=journal
SyslogIdentifier=raa-server

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now "${SERVICE_NAME}"

# ── Enable linger (boot startup without login) ────────────────────────────────
if ! loginctl show-user "$USER" 2>/dev/null | grep -q 'Linger=yes'; then
    echo "==> Enabling linger for $USER (requires sudo, once)..."
    sudo loginctl enable-linger "$USER"
fi

# ── Done ───────────────────────────────────────────────────────────────────────
echo ""
echo "Installation complete. Service is running."
echo ""
echo "  systemctl --user status ${SERVICE_NAME}"
echo "  systemctl --user restart ${SERVICE_NAME}"
echo "  journalctl --user -u ${SERVICE_NAME} -f"
