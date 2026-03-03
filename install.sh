#!/bin/bash
# QuantumSync Local — Installation Script
# Installs standalone music player on Raspberry Pi
# Also cleans up old QuantumSync client/server files

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}╔══════════════════════════════════════╗${NC}"
echo -e "${GREEN}║   QuantumSync Local — Installer      ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════╝${NC}"
echo ""

# Must run as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Please run as root: sudo ./install.sh${NC}"
    exit 1
fi

# ──────────────────────────────────────────────
# Phase 1: Clean up old QuantumSync
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 1] Cleaning up old QuantumSync...${NC}"

# Stop and disable old services
for svc in quantumsync-client quantumsync-server quantumsync-mpd-watchdog; do
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
        echo "  Stopping $svc..."
        systemctl stop "$svc" 2>/dev/null || true
    fi
    if systemctl is-enabled --quiet "$svc" 2>/dev/null; then
        echo "  Disabling $svc..."
        systemctl disable "$svc" 2>/dev/null || true
    fi
done

# Remove old systemd service files
for f in /etc/systemd/system/quantumsync-client.service \
         /etc/systemd/system/quantumsync-server.service \
         /etc/systemd/system/quantumsync-mpd-watchdog.service; do
    if [ -f "$f" ]; then
        echo "  Removing $f"
        rm -f "$f"
    fi
done

# Remove old binaries
for f in /usr/local/bin/quantumsync-client \
         /usr/local/bin/quantumsync-server \
         /usr/local/bin/quantumsync-music-player \
         /usr/local/bin/quantumsync-mpd-watchdog; do
    if [ -f "$f" ]; then
        echo "  Removing $f"
        rm -f "$f"
    fi
done

# Remove old config (but keep music if present)
if [ -d "/etc/quantumsync" ]; then
    echo "  Removing /etc/quantumsync/"
    rm -rf /etc/quantumsync
fi

# Remove old web GUI files
if [ -d "/usr/share/quantumsync" ]; then
    echo "  Removing /usr/share/quantumsync/"
    rm -rf /usr/share/quantumsync
fi

# Remove old build artifacts if repo exists
if [ -d "/home/pi/quantum_sync_V3" ]; then
    echo "  Removing /home/pi/quantum_sync_V3/"
    rm -rf /home/pi/quantum_sync_V3
fi
if [ -d "/home/pi/quantum_sync_V2" ]; then
    echo "  Removing /home/pi/quantum_sync_V2/"
    rm -rf /home/pi/quantum_sync_V2
fi

# Reload systemd after removing service files
systemctl daemon-reload

echo -e "${GREEN}  Old QuantumSync cleaned up.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 2: Install dependencies
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 2] Installing dependencies...${NC}"

apt-get update -qq
apt-get install -y -qq build-essential cmake libboost-all-dev mpd mpc

echo -e "${GREEN}  Dependencies installed.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 3: Create user and directories
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 3] Setting up user and directories...${NC}"

# Create quantumsync user if it doesn't exist
if ! id -u quantumsync &>/dev/null; then
    useradd -r -s /sbin/nologin -d /var/lib/quantumsync -m quantumsync
    echo "  Created user: quantumsync"
fi

# Add to audio and systemd-journal groups
usermod -a -G audio quantumsync 2>/dev/null || true
usermod -a -G systemd-journal quantumsync 2>/dev/null || true

# Create directories
mkdir -p /opt/quantumsync-local/music
mkdir -p /var/lib/quantumsync-local/mpd/playlists
mkdir -p /etc/quantumsync-local
mkdir -p /run/quantumsync-local

# Set ownership
chown -R quantumsync:quantumsync /opt/quantumsync-local
chown -R quantumsync:quantumsync /var/lib/quantumsync-local
chown -R quantumsync:quantumsync /etc/quantumsync-local
chown -R quantumsync:quantumsync /run/quantumsync-local

echo -e "${GREEN}  User and directories ready.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 4: Build C++ program
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 4] Building quantumsync-local...${NC}"

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"

# Install binary
cp quantumsync-local /usr/local/bin/quantumsync-local
chmod 755 /usr/local/bin/quantumsync-local

cd "$SCRIPT_DIR"
echo -e "${GREEN}  Build complete. Binary: /usr/local/bin/quantumsync-local${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 5: Configure MPD
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 5] Configuring MPD...${NC}"

# Stop system MPD if running (we use our own instance)
systemctl stop mpd 2>/dev/null || true
systemctl disable mpd 2>/dev/null || true

# Install our MPD config
cp "$SCRIPT_DIR/config/mpd.conf" /etc/quantumsync-local/mpd.conf
chown quantumsync:quantumsync /etc/quantumsync-local/mpd.conf

echo -e "${GREEN}  MPD configured.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 6: Device name configuration
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 6] Device configuration...${NC}"

# Check if config already exists (re-install)
if [ -f /etc/quantumsync-local/config.conf ]; then
    EXISTING_NAME=$(grep "^DEVICE_NAME=" /etc/quantumsync-local/config.conf | cut -d'=' -f2)
    echo "  Existing config found: DEVICE_NAME=$EXISTING_NAME"
    read -p "  Keep existing name? [Y/n]: " KEEP_NAME
    if [ "$KEEP_NAME" = "n" ] || [ "$KEEP_NAME" = "N" ]; then
        EXISTING_NAME=""
    fi
fi

if [ -z "$EXISTING_NAME" ]; then
    read -p "  Enter device/room name (e.g. Kitchen, Lounge): " DEVICE_NAME
    if [ -z "$DEVICE_NAME" ]; then
        DEVICE_NAME="Local Player"
    fi
else
    DEVICE_NAME="$EXISTING_NAME"
fi

cat > /etc/quantumsync-local/config.conf << EOF
# QuantumSync Local Configuration
DEVICE_NAME=$DEVICE_NAME
HTTP_PORT=1706
EOF

chown quantumsync:quantumsync /etc/quantumsync-local/config.conf

echo -e "${GREEN}  Device name: $DEVICE_NAME${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 7: Install and enable systemd services
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 7] Installing systemd services...${NC}"

# Install service files
cp "$SCRIPT_DIR/systemd/quantumsync-local-mpd.service" /etc/systemd/system/
cp "$SCRIPT_DIR/systemd/quantumsync-local.service" /etc/systemd/system/

# Create tmpfiles.d entry for /run directory (survives reboots)
cat > /etc/tmpfiles.d/quantumsync-local.conf << EOF
d /run/quantumsync-local 0755 quantumsync quantumsync -
EOF

systemctl daemon-reload

# Enable and start services
systemctl enable quantumsync-local-mpd.service
systemctl enable quantumsync-local.service

systemctl start quantumsync-local-mpd.service
sleep 3
systemctl start quantumsync-local.service

echo -e "${GREEN}  Services installed and started.${NC}"
echo ""

# ──────────────────────────────────────────────
# Phase 8: Initialize MPD database
# ──────────────────────────────────────────────
echo -e "${YELLOW}[Phase 8] Initializing MPD...${NC}"

# Wait for MPD to be ready
sleep 2
mpc update --wait 2>/dev/null || true

# Count tracks
TRACK_COUNT=$(mpc listall 2>/dev/null | wc -l)
echo "  Found $TRACK_COUNT tracks in music directory"

if [ "$TRACK_COUNT" -gt 0 ]; then
    mpc clear 2>/dev/null || true
    mpc add / 2>/dev/null || true
    mpc repeat on 2>/dev/null || true
    mpc random on 2>/dev/null || true
    mpc play 2>/dev/null || true
    echo -e "${GREEN}  Music playing!${NC}"
else
    echo -e "${YELLOW}  No music files found. Copy music to /opt/quantumsync-local/music/${NC}"
    echo "  Then run: mpc update && mpc add / && mpc play"
fi

echo ""

# ──────────────────────────────────────────────
# Done!
# ──────────────────────────────────────────────
IP_ADDR=$(hostname -I | awk '{print $1}')

echo -e "${GREEN}╔══════════════════════════════════════╗${NC}"
echo -e "${GREEN}║   Installation Complete!             ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════╝${NC}"
echo ""
echo "  Device:    $DEVICE_NAME"
echo "  Web GUI:   http://$IP_ADDR:1706/"
echo "  Music dir: /opt/quantumsync-local/music/"
echo ""
echo "  To add music:"
echo "    scp *.mp3 pi@$IP_ADDR:/opt/quantumsync-local/music/"
echo "    Then: mpc update && mpc add / && mpc play"
echo ""
echo "  Service commands:"
echo "    systemctl status quantumsync-local"
echo "    systemctl status quantumsync-local-mpd"
echo "    journalctl -u quantumsync-local -f"
echo ""
