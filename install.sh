#!/usr/bin/env bash
set -euo pipefail

echo "========================================"
echo " ANE WebRTC Sensor – Dependency Installer"
echo " Raspberry Pi OS (Debian) – GI visible in venv"
echo "========================================"

echo "[1/7] Installing system dependencies (APT)..."
sudo apt update
sudo apt install -y \
  ca-certificates \
  build-essential pkg-config libc6-dev \
  python3 python3-full python3-venv python3-pip python3-dev \
  python3-gi python3-gst-1.0 \
  gir1.2-gstreamer-1.0 \
  gir1.2-gst-plugins-base-1.0 \
  gir1.2-gst-plugins-bad-1.0 \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  gstreamer1.0-alsa \
  libopus-dev libopus0

# (Opcional si usas zmq en Python)
sudo apt install -y python3-zmq || true

echo "[2/7] Verifying gi on system python..."
python3 -c "import gi; print('✔ gi system OK:', gi.__file__)" >/dev/null

VENV_DIR="./venv"
VENV_PY="${VENV_DIR}/bin/python"

echo "[3/7] Creating venv with --system-site-packages..."
rm -rf "$VENV_DIR"
/usr/bin/python3 -m venv --system-site-packages "$VENV_DIR"

echo "[4/7] Verifying gi inside venv..."
"$VENV_PY" -c "import gi; print('✔ gi venv OK:', gi.__file__)" >/dev/null

echo "[5/7] Installing Python packages into venv (PEP668-safe)..."
"$VENV_PY" -m ensurepip --upgrade >/dev/null 2>&1 || true
"$VENV_PY" -m pip install --upgrade pip
"$VENV_PY" -m pip install websockets==15.0.1
"$VENV_PY" -m pip install pyzmq || true

echo "[6/7] Verifying GStreamer plugins..."
gst-inspect-1.0 webrtcbin >/dev/null || { echo "[ERROR] webrtcbin NOT found"; exit 1; }
gst-inspect-1.0 opusparse >/dev/null || { echo "[ERROR] opusparse NOT found"; exit 1; }
gst-inspect-1.0 rtpopuspay >/dev/null || { echo "[ERROR] rtpopuspay NOT found"; exit 1; }

echo "[7/7] Verifying GstWebRTC bindings in venv..."
"$VENV_PY" - <<'EOF'
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gst, GstWebRTC, GstSdp, GLib
Gst.init(None)
print("✔ GstWebRTC available in venv")
print("✔ GStreamer:", Gst.version_string())
EOF

test -f /usr/include/opus/opus.h || { echo "[ERROR] Opus headers not found: /usr/include/opus/opus.h"; exit 1; }

echo "========================================"
echo " OK."
echo " Run with:"
echo "   ./venv/bin/python server_webrtc.py"
echo "========================================"

