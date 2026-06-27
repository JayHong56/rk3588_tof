#!/usr/bin/env bash

set -euo pipefail

CAMERA_HOST=${1:-${TOF_CAMERA_IP:-10.42.0.1}}
CAMERA_USER=${TOF_CAMERA_USER:-analog}
CAMERA_SERVICE=${TOF_CAMERA_SERVICE:-adi-tof.service}
CAMERA_PORT=${TOF_CAMERA_PORT:-5000}
WAIT_SECONDS=${TOF_CAMERA_RESTART_WAIT_SECONDS:-30}

usage() {
    cat <<EOF
Usage: $(basename "$0") [CAMERA_IP]

Restart the ToF server over SSH, show its systemd status, and wait for the
SDK TCP port to become reachable.

Environment overrides:
  TOF_CAMERA_IP                    default: 10.42.0.1
  TOF_CAMERA_USER                  default: analog
  TOF_CAMERA_SERVICE               default: adi-tof.service
  TOF_CAMERA_PORT                  default: 5000
  TOF_CAMERA_RESTART_WAIT_SECONDS  default: 30
EOF
}

case ${1:-} in
-h|--help)
    usage
    exit 0
    ;;
esac

for tool in ssh nc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing required command: $tool" >&2
        exit 1
    fi
done

if [[ ! "$CAMERA_SERVICE" =~ ^[A-Za-z0-9_.@-]+$ ]]; then
    echo "Invalid systemd service name: $CAMERA_SERVICE" >&2
    exit 2
fi
if [[ ! "$CAMERA_PORT" =~ ^[0-9]+$ ]] || ((CAMERA_PORT < 1 || CAMERA_PORT > 65535)); then
    echo "Invalid camera port: $CAMERA_PORT" >&2
    exit 2
fi
if [[ ! "$WAIT_SECONDS" =~ ^[0-9]+$ ]] || ((WAIT_SECONDS < 1)); then
    echo "Invalid restart wait time: $WAIT_SECONDS" >&2
    exit 2
fi

target="${CAMERA_USER}@${CAMERA_HOST}"

echo "Restarting $CAMERA_SERVICE on $target ..."
if ! ssh -tt \
    -o StrictHostKeyChecking=accept-new \
    -o ConnectTimeout=5 \
    "$target" \
    "sudo systemctl restart '$CAMERA_SERVICE' && sudo systemctl --no-pager --full status '$CAMERA_SERVICE'"; then
    echo "Failed to restart the camera service over SSH." >&2
    echo "If SSH is also unresponsive, power-cycle the camera board." >&2
    exit 1
fi

echo "Waiting up to ${WAIT_SECONDS}s for ${CAMERA_HOST}:${CAMERA_PORT} ..."
deadline=$((SECONDS + WAIT_SECONDS))
while ((SECONDS < deadline)); do
    if nc -z -w 1 "$CAMERA_HOST" "$CAMERA_PORT" >/dev/null 2>&1; then
        echo "Camera service is ready: ${CAMERA_HOST}:${CAMERA_PORT}"
        exit 0
    fi
    sleep 1
done

echo "Service restarted, but ${CAMERA_HOST}:${CAMERA_PORT} did not become reachable." >&2
exit 1
