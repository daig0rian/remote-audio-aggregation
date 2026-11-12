#!/usr/bin/env bash
# raa-server installer
# Usage:
#   bash install.sh          # installs latest release
#   bash install.sh v1.2.3   # installs specific version
#
# Installs to ~/raa-server and registers a systemd user service.
# Requires Node.js >= 22 (the OS package `nodejs` works if it is >= 22; nvm also works).
# The release tarball bundles node_modules (incl. prebuilt @evan/opus binaries),
# so no npm install / registry.npmjs.org access / build tools are needed.
# The only operation requiring sudo is loginctl enable-linger
# (once, so the service starts at boot without login).
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
for cmd in node curl tar; do
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "ERROR: '$cmd' not found."
        echo "  Install Node.js >= 22 (OS package: sudo apt install nodejs, or nvm: https://github.com/nvm-sh/nvm)"
        exit 1
    }
done

NODE_MAJOR=$(node -e 'process.stdout.write(process.versions.node.split(".")[0])')
if [[ "$NODE_MAJOR" -lt 22 ]]; then
    echo "ERROR: Node.js >= 22 required (found $(node --version))."
    echo "  If the OS package is older, install via nvm:"
    echo "  nvm install 22 && nvm alias default 22"
    exit 1
fi

NODE_BIN=$(command -v node)
echo "==> Using Node.js $(node --version) at ${NODE_BIN}"

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
# Remove node_modules from a previous install so stale files from older
# dependency versions don't get mixed with the bundled ones.
rm -rf "$INSTALL_DIR/node_modules"
# npm pack puts files under package/ — strip that prefix
tar -xzf "$TMP/raa-server.tgz" -C "$INSTALL_DIR" --strip-components=1
if [[ ! -d "$INSTALL_DIR/node_modules" ]]; then
    echo "ERROR: node_modules is not bundled in raa-server-${VERSION}.tgz."
    echo "  Releases before bundled node_modules require: cd $INSTALL_DIR && npm install --omit=dev"
    exit 1
fi

# ── Download raa-client.exe (for WebUI distribution on closed LAN) ────────────
CLIENT_URL="https://github.com/${REPO}/releases/download/v${VERSION}/raa-client.exe"
echo "==> Downloading raa-client.exe ..."
if curl -fsSL "$CLIENT_URL" -o "$INSTALL_DIR/public/raa-client.exe"; then
    echo "    → $INSTALL_DIR/public/raa-client.exe"
else
    echo "WARN: raa-client.exe download failed — client download will not be available in WebUI"
fi

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
