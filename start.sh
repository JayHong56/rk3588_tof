#!/usr/bin/env bash
set -euo pipefail

cd /home/pi/rk3588_tof

export DISPLAY=:10
TOF_PASSWORD="${TOF_PASSWORD:-analog}" \
AUTO_PLAY="${AUTO_PLAY:-1}" \
MODE="${MODE:-lr-mixed}" \
FPS="${FPS:-20}" \
VIEWER_DIR="${VIEWER_DIR:-/home/pi/rk3588_tof/tof_net/build-pi/examples/tof-net-viewer}" \
SAVE_PROCESSED="${SAVE_PROCESSED:-1}" \
SAVE_PROCESSED_DIR="${SAVE_PROCESSED_DIR:-/home/pi/rk3588_tof/processed_frames}" \
SAVE_PROCESSED_PLANES="${SAVE_PROCESSED_PLANES:-depth,ir}" \
SAVE_PROCESSED_STRIDE="${SAVE_PROCESSED_STRIDE:-1}" \
SAVE_PROCESSED_MAX_FRAMES="${SAVE_PROCESSED_MAX_FRAMES:-0}" \
/home/pi/rk3588_tof/camera-logs/start_tof_net_manual_play.sh "$@"
