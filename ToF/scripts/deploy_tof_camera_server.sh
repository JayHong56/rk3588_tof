#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-/tmp/rk3588_tof_server_build}"
DEPLOY_DIR="${DEPLOY_DIR:-$ROOT_DIR/deploy}"
CAMERA_HOST="${TOF_CAMERA_IP:-10.42.0.1}"
CAMERA_USER="${TOF_CAMERA_USER:-analog}"
CAMERA_SERVICE="${TOF_CAMERA_SERVICE:-network-gadget.service}"
PATCH_MARKER="${PATCH_MARKER:-rk3588-tof-getframe-watchdog-20260625}"
SKIP_BUILD=0

usage() {
    cat <<USAGE
Usage: $0 [options]

Build and deploy the patched aditof-server to the camera host.

Options:
  --host HOST       Camera host/IP. Default: $CAMERA_HOST
  --user USER       SSH user. Default: $CAMERA_USER
  --skip-build      Reuse files already present in deploy/
  -h, --help        Show this help

Environment:
  TOF_CAMERA_IP, TOF_CAMERA_USER, TOF_CAMERA_SERVICE, BUILD_DIR, DEPLOY_DIR
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --host)
        CAMERA_HOST="$2"
        shift 2
        ;;
    --user)
        CAMERA_USER="$2"
        shift 2
        ;;
    --skip-build)
        SKIP_BUILD=1
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

SERVER_SRC="$BUILD_DIR/apps/server/aditof-server"
LIB_SRC="$BUILD_DIR/sdk/libaditof.so.4.2.0"
SERVICE_SRC="$ROOT_DIR/ToF/sdcard-images-utils/nxp/patches/ubuntu_overlay/step1/usr/lib/systemd/system/network-gadget.service"
REMOTE="$CAMERA_USER@$CAMERA_HOST"

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    echo "[1/6] Configuring camera-server build in $BUILD_DIR"
    cmake -S "$ROOT_DIR/ToF" -B "$BUILD_DIR" \
        -DNXP=ON \
        -DWITH_EXAMPLES=OFF \
        -DWITH_DOC=OFF \
        -DWITH_PYTHON=OFF \
        -DWITH_OPENCV=OFF \
        -DWITH_OPEN3D=OFF \
        -DWITH_ROS=OFF \
        -DWITH_ROS2=OFF \
        -DWITH_NETWORK=ON \
        -DWITH_EMBEDDED_LINUX_TOOLS=OFF \
        -DUSE_DEPTH_COMPUTE_OPENSOURCE=ON

    echo "[2/6] Building aditof-server"
    cmake --build "$BUILD_DIR" --target aditof-server -j2
else
    echo "[1/6] Skipping build"
fi

mkdir -p "$DEPLOY_DIR"

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    cp "$SERVER_SRC" "$DEPLOY_DIR/aditof-server"
    cp "$LIB_SRC" "$DEPLOY_DIR/libaditof.so.4.2.0"
    cp "$SERVICE_SRC" "$DEPLOY_DIR/network-gadget.service"
fi

SERVER_DEPLOY="$DEPLOY_DIR/aditof-server"
LIB_DEPLOY="$DEPLOY_DIR/libaditof.so.4.2.0"
SERVICE_DEPLOY="$DEPLOY_DIR/network-gadget.service"
REMOTE_INSTALLER="$DEPLOY_DIR/install_tof_camera_server_remote.sh"

for path in "$SERVER_DEPLOY" "$LIB_DEPLOY" "$SERVICE_DEPLOY"; do
    if [[ ! -e "$path" ]]; then
        echo "Missing deploy artifact: $path" >&2
        exit 1
    fi
done

SERVER_STRINGS="$(strings "$SERVER_DEPLOY")"
if ! grep -q "$PATCH_MARKER" <<<"$SERVER_STRINGS"; then
    echo "Deploy server does not contain patch marker: $PATCH_MARKER" >&2
    exit 1
fi

SERVER_SHA="$(sha256sum "$SERVER_DEPLOY" | awk '{print $1}')"
LIB_SHA="$(sha256sum "$LIB_DEPLOY" | awk '{print $1}')"

echo "[3/6] Local artifacts"
echo "  server: $SERVER_DEPLOY"
echo "  server sha256: $SERVER_SHA"
echo "  libaditof: $LIB_DEPLOY"
echo "  libaditof sha256: $LIB_SHA"

cat >"$REMOTE_INSTALLER" <<'REMOTE_SCRIPT'
#!/usr/bin/env bash
set -euo pipefail

SERVICE="$1"
PATCH_MARKER="$2"
EXPECTED_SERVER_SHA="$3"

SERVER_DST=/usr/share/systemd/aditof-server
SERVICE_DST=/lib/systemd/system/network-gadget.service
LIB_DIR=/home/analog/Workspace/ToF/build/sdk
LD_CONF=/etc/ld.so.conf.d/aditof.conf
TS=$(date +%Y%m%d_%H%M%S)

echo "[remote] Stopping service/path"
sudo systemctl stop network-gadget.path 2>/dev/null || true
sudo systemctl stop "$SERVICE" 2>/dev/null || true
sudo pkill -KILL -x aditof-server 2>/dev/null || true

echo "[remote] Removing temporary debug override if present"
sudo rm -f /etc/systemd/system/network-gadget.service.d/debug-no-restart.conf

echo "[remote] Creating required library directories"
sudo mkdir -p "$LIB_DIR"

echo "[remote] Backing up existing files"
if [[ -e "$SERVER_DST" ]]; then
    sudo cp "$SERVER_DST" "$SERVER_DST.bak.$TS"
fi
if [[ -e "$SERVICE_DST" ]]; then
    sudo cp "$SERVICE_DST" "$SERVICE_DST.bak.$TS"
fi
if [[ -e "$LIB_DIR/libaditof.so.4.2.0" ]]; then
    sudo cp "$LIB_DIR/libaditof.so.4.2.0" "$LIB_DIR/libaditof.so.4.2.0.bak.$TS"
fi

echo "[remote] Installing server, library, and service"
sudo install -m 755 /tmp/aditof-server "$SERVER_DST"
sudo install -m 755 /tmp/libaditof.so.4.2.0 "$LIB_DIR/libaditof.so.4.2.0"
sudo ln -sfn libaditof.so.4.2.0 "$LIB_DIR/libaditof.so.1.0"
sudo ln -sfn libaditof.so.1.0 "$LIB_DIR/libaditof.so"
sudo install -m 644 /tmp/network-gadget.service "$SERVICE_DST"

echo "[remote] Writing ldconfig paths"
sudo tee "$LD_CONF" >/dev/null <<EOF
$LIB_DIR
$LIB_DIR/common/adi/depth-compute-stub
$LIB_DIR/common/adi/depth-compute-opensource
/opt/glog/lib
/opt/websockets/lib
EOF

sudo ldconfig
sudo systemctl daemon-reload

echo "[remote] Verifying deployed server"
ACTUAL_SERVER_SHA="$(sha256sum "$SERVER_DST" | awk '{print $1}')"
echo "[remote] server sha256: $ACTUAL_SERVER_SHA"
if [[ "$ACTUAL_SERVER_SHA" != "$EXPECTED_SERVER_SHA" ]]; then
    echo "[remote] ERROR: server sha mismatch" >&2
    exit 1
fi
SERVER_STRINGS="$(strings "$SERVER_DST")"
if ! grep -q "$PATCH_MARKER" <<<"$SERVER_STRINGS"; then
    echo "[remote] ERROR: missing patch marker $PATCH_MARKER" >&2
    exit 1
fi
if ldd "$SERVER_DST" | grep -q 'not found'; then
    ldd "$SERVER_DST" >&2
    echo "[remote] ERROR: missing runtime libraries" >&2
    exit 1
fi

echo "[remote] Starting $SERVICE"
sudo systemctl start "$SERVICE"
sleep 2

echo "[remote] Recent service log"
sudo journalctl -u "$SERVICE" -b -n 80 --no-pager

SERVICE_LOG="$(sudo journalctl -u "$SERVICE" -b --no-pager)"
if ! grep -q "Server patch marker: $PATCH_MARKER" <<<"$SERVICE_LOG"; then
    echo "[remote] ERROR: patch marker was not logged after service start" >&2
    exit 1
fi

echo "[remote] Enabling path trigger"
sudo systemctl start network-gadget.path 2>/dev/null || true

echo "[remote] Final state"
systemctl is-active "$SERVICE"
systemctl is-active network-gadget.path 2>/dev/null || true
pgrep -af aditof-server || true
REMOTE_SCRIPT
chmod +x "$REMOTE_INSTALLER"

echo "[4/6] Copying artifacts to $REMOTE:/tmp"
scp "$SERVER_DEPLOY" "$LIB_DEPLOY" "$SERVICE_DEPLOY" "$REMOTE_INSTALLER" "$REMOTE:/tmp/"

echo "[5/6] Installing on camera host"
ssh -tt "$REMOTE" "sudo bash /tmp/install_tof_camera_server_remote.sh '$CAMERA_SERVICE' '$PATCH_MARKER' '$SERVER_SHA'"

echo "[6/6] Deployment completed"
cat <<EOF

To follow camera logs from RK3588:

  mkdir -p "$ROOT_DIR/camera-logs"
  log="$ROOT_DIR/camera-logs/camera_\$(date +%Y%m%d_%H%M%S).log"
  ssh -tt "$REMOTE" 'sudo journalctl -b -f -o short-precise' 2>&1 | tee "\$log"

Expected service log marker:
  Server patch marker: $PATCH_MARKER

Expected GetFrame phase logs during viewer playback:
  GetFrame: waitForBuffer begin
  GetFrame: VIDIOC_DQBUF begin
  GetFrame: getInternalBuffer begin
  GetFrame: VIDIOC_QBUF begin
EOF
