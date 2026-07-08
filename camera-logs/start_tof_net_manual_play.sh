#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-"$HOME/rk3588_tof"}
LOG_DIR=${LOG_DIR:-"$ROOT/camera-logs/new_logs"}
VIEWER_DIR=${VIEWER_DIR:-"$ROOT/tof_net/build/examples/tof-net-viewer"}
VIEWER_BIN=${VIEWER_BIN:-"$VIEWER_DIR/tof-net-viewer"}

TOF_USER=${TOF_USER:-analog}
TOF_HOST=${TOF_HOST:-10.42.0.1}
TOF_TARGET=${TOF_TARGET:-"$TOF_USER@$TOF_HOST"}
TOF_COLLECT_DIR=${TOF_COLLECT_DIR:-/home/analog/rk3588_tof/tof_net/build/examples/tof-net-collect}
TOF_COLLECT_CONFIG=${TOF_COLLECT_CONFIG:-config/config_crosby_adsd3500_new_modes.json}

SERVER_IP=${SERVER_IP:-10.42.0.103}
PORT=${PORT:-5000}
MODE=${MODE:-${FRAME_TYPE:-lr-mixed}}
N_FRAMES=${N_FRAMES:-0}
FPS=${FPS:-0}
DISPLAY=${DISPLAY:-:0}
METADATA_CACHE=${METADATA_CACHE:-1}
METADATA_CACHE_DIR=${METADATA_CACHE_DIR:-/home/analog/tof_net_metadata_cache}
REFRESH_METADATA=${REFRESH_METADATA:-0}
SEND_ALL_DEALIAS=${SEND_ALL_DEALIAS:-0}

WAIT_METADATA_SECONDS=${WAIT_METADATA_SECONDS:-90}
SSH_CONNECT_TIMEOUT=${SSH_CONNECT_TIMEOUT:-8}
AUTO_PLAY=${AUTO_PLAY:-0}
SAVE_PROCESSED=${SAVE_PROCESSED:-0}
SAVE_PROCESSED_DIR=${SAVE_PROCESSED_DIR:-"$ROOT/processed_frames"}
SAVE_PLANES=${SAVE_PLANES:-${SAVE_PROCESSED_PLANES:-depth,ir}}
SAVE_PROCESSED_PLANES=${SAVE_PROCESSED_PLANES:-$SAVE_PLANES}
SAVE_PROCESSED_STRIDE=${SAVE_PROCESSED_STRIDE:-1}
SAVE_PROCESSED_MAX_FRAMES=${SAVE_PROCESSED_MAX_FRAMES:-0}

usage() {
    cat <<EOF
Usage:
  $(basename "$0")

Starts tof-net-viewer on RK, starts tof_net_collect on ToF, then waits for the
metadata handshake. By default, click Play manually in the GUI to start capture.
Set AUTO_PLAY=1 to request Play from inside the GUI main loop after metadata is
ready; this uses the same PlayCCD path as the button, without mouse injection.

Environment overrides:
  TOF_HOST=$TOF_HOST
  TOF_USER=$TOF_USER
  SERVER_IP=$SERVER_IP
  PORT=$PORT
  MODE=$MODE              # accepts mode id or name: 5, lr-mixed, sr-native, ...
  FRAME_TYPE=<mode-name>  # legacy alias used only when MODE is unset
  N_FRAMES=$N_FRAMES
  FPS=$FPS
  METADATA_CACHE=$METADATA_CACHE
  METADATA_CACHE_DIR=$METADATA_CACHE_DIR
  REFRESH_METADATA=$REFRESH_METADATA
  SEND_ALL_DEALIAS=$SEND_ALL_DEALIAS
  DISPLAY=$DISPLAY
  LOG_DIR=$LOG_DIR
  AUTO_PLAY=$AUTO_PLAY
  SAVE_PROCESSED=$SAVE_PROCESSED
  SAVE_PROCESSED_DIR=$SAVE_PROCESSED_DIR
  SAVE_PLANES=$SAVE_PLANES        # raw,depth,ir,xyz; legacy SAVE_PROCESSED_PLANES also works
  SAVE_PROCESSED_PLANES=$SAVE_PROCESSED_PLANES
  SAVE_PROCESSED_STRIDE=$SAVE_PROCESSED_STRIDE
  SAVE_PROCESSED_MAX_FRAMES=$SAVE_PROCESSED_MAX_FRAMES
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

die() {
    echo "error: $*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

need_cmd ssh
need_cmd setsid
need_cmd ip

[[ -x "$VIEWER_BIN" ]] || die "viewer binary not executable: $VIEWER_BIN"
[[ -d "$LOG_DIR" ]] || mkdir -p "$LOG_DIR"

mode_name_from_input() {
    local value
    value=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')
    case "$value" in
        0|sr-native) echo "sr-native" ;;
        1|lr-native) echo "lr-native" ;;
        2|sr-qnative) echo "sr-qnative" ;;
        3|lr-qnative) echo "lr-qnative" ;;
        4|pcm-native|pcm) echo "pcm-native" ;;
        5|lr-mixed) echo "lr-mixed" ;;
        6|sr-mixed) echo "sr-mixed" ;;
        *) die "unsupported MODE '$1' (valid: 0..6, sr-native, lr-native, sr-qnative, lr-qnative, pcm-native, lr-mixed, sr-mixed)" ;;
    esac
}

mode_id_from_name() {
    case "$1" in
        sr-native) echo "0" ;;
        lr-native) echo "1" ;;
        sr-qnative) echo "2" ;;
        lr-qnative) echo "3" ;;
        pcm-native) echo "4" ;;
        lr-mixed) echo "5" ;;
        sr-mixed) echo "6" ;;
        *) die "internal error: no mode id for '$1'" ;;
    esac
}

FRAME_TYPE=$(mode_name_from_input "$MODE")
MODE_ID=$(mode_id_from_name "$FRAME_TYPE")
[[ "$FPS" =~ ^[0-9]+$ ]] || die "unsupported FPS '$FPS' (must be a non-negative integer; 0 uses config/default)"
[[ "$SAVE_PROCESSED_STRIDE" =~ ^[1-9][0-9]*$ ]] || die "unsupported SAVE_PROCESSED_STRIDE '$SAVE_PROCESSED_STRIDE' (must be >= 1)"
[[ "$SAVE_PROCESSED_MAX_FRAMES" =~ ^[0-9]+$ ]] || die "unsupported SAVE_PROCESSED_MAX_FRAMES '$SAVE_PROCESSED_MAX_FRAMES' (must be >= 0)"

flag_enabled() {
    [[ "$1" == "1" || "$1" == "true" || "$1" == "TRUE" || "$1" == "on" || "$1" == "ON" || "$1" == "yes" || "$1" == "YES" ]]
}

if [[ -z "${TOF_PASSWORD:-}" ]]; then
    read -rsp "ToF SSH/sudo password [$TOF_USER default: analog]: " TOF_PASSWORD
    echo
    TOF_PASSWORD=${TOF_PASSWORD:-analog}
fi
export TOF_PASSWORD

ts=$(date +%Y%m%d_%H%M%S)
viewer_log="$LOG_DIR/viewer_terminal_${ts}_manual_play_default.log"
tof_log_remote="/home/analog/tof_log_all/tof_collect_${ts}_manual_play_default.log"
tof_log_local="$LOG_DIR/tof_collect_${ts}_manual_play_default.log"
rk_usb_dmesg_log="$LOG_DIR/rk_usb_dmesg_${ts}_manual_play_default.log"
rk_usb_link_log="$LOG_DIR/rk_usb_link_${ts}_manual_play_default.log"
tof_password_q=$(printf '%q' "$TOF_PASSWORD")

viewer_pid=""
ssh_collect_pid=""
rk_dmesg_pid=""
rk_link_pid=""

ssh_with_password() {
    local target=$1
    local remote_cmd=$2
    local local_log=${3:-/dev/null}
    local askpass
    askpass=$(mktemp "${TMPDIR:-/tmp}/tof_ssh_askpass.XXXXXX")
    chmod 700 "$askpass"
    cat >"$askpass" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$TOF_PASSWORD"
EOF
    set +e
    DISPLAY=${DISPLAY:-:0} \
    SSH_ASKPASS="$askpass" \
    SSH_ASKPASS_REQUIRE=force \
    setsid -w ssh \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile="$HOME/.ssh/known_hosts" \
        -o ConnectTimeout="$SSH_CONNECT_TIMEOUT" \
        -o ConnectionAttempts=1 \
        -o ServerAliveInterval=3 \
        -o ServerAliveCountMax=2 \
        "$target" "$remote_cmd" >"$local_log" 2>&1
    local status=$?
    set -e
    rm -f "$askpass"
    return "$status"
}

stop_remote_collect() {
    ssh_with_password "$TOF_TARGET" "printf '%s\n' $tof_password_q | sudo -S pkill -INT -f tof_net_collect || true" "$LOG_DIR/remote_stop_${ts}.log" || true
}

start_rk_monitors() {
    (
        echo "# $(date -Is) RK dmesg -wT"
        dmesg -wT
    ) >"$rk_usb_dmesg_log" 2>&1 &
    rk_dmesg_pid=$!

    (
        while true; do
            echo "===== $(date -Is) ====="
            ip -brief addr || true
            ip route || true
            if command -v ss >/dev/null 2>&1; then
                ss -tanp 2>/dev/null | grep -E "($TOF_HOST|$SERVER_IP|:$PORT)" || true
            fi
            sleep 1
        done
    ) >"$rk_usb_link_log" 2>&1 &
    rk_link_pid=$!
}

wait_local_server_ip() {
    local timeout=$1
    local start
    start=$(date +%s)
    while true; do
        if ip -brief addr | grep -Fq "$SERVER_IP/"; then
            return 0
        fi
        if (( $(date +%s) - start >= timeout )); then
            return 1
        fi
        sleep 0.5
    done
}

cleanup() {
    echo
    echo "Stopping..."
    if [[ -n "$viewer_pid" ]] && kill -0 "$viewer_pid" 2>/dev/null; then
        kill -INT "$viewer_pid" 2>/dev/null || true
        sleep 1
        kill "$viewer_pid" 2>/dev/null || true
    fi
    if [[ -n "$ssh_collect_pid" ]] && kill -0 "$ssh_collect_pid" 2>/dev/null; then
        kill -INT "$ssh_collect_pid" 2>/dev/null || true
    fi
    if [[ -n "$rk_dmesg_pid" ]] && kill -0 "$rk_dmesg_pid" 2>/dev/null; then
        kill "$rk_dmesg_pid" 2>/dev/null || true
    fi
    if [[ -n "$rk_link_pid" ]] && kill -0 "$rk_link_pid" 2>/dev/null; then
        kill "$rk_link_pid" 2>/dev/null || true
    fi
    stop_remote_collect
    if [[ -f "$tof_log_local" ]]; then
        echo "ToF collect log: $tof_log_local"
    fi
    echo "Viewer log: $viewer_log"
}
trap cleanup INT TERM EXIT

wait_log() {
    local pattern=$1
    local file=$2
    local timeout=$3
    local start
    start=$(date +%s)
    while true; do
        if [[ -f "$file" ]] && grep -Fq "$pattern" "$file"; then
            return 0
        fi
        if (( $(date +%s) - start >= timeout )); then
            return 1
        fi
        sleep 0.5
    done
}

echo "Stopping stale processes..."
pkill -INT -f tof-net-viewer 2>/dev/null || true
stop_remote_collect

if ! wait_local_server_ip 20; then
    die "RK USB IP $SERVER_IP is not present; check usb0 before starting"
fi

if command -v ping >/dev/null 2>&1 && ! ping -c 1 -W 1 "$TOF_HOST" >/dev/null 2>&1; then
    echo "warning: $TOF_HOST did not answer one ping; SSH may still succeed after network-gadget settles" >&2
fi

echo "Starting RK USB/link monitors..."
start_rk_monitors

echo "Starting viewer..."
viewer_args=()
if flag_enabled "$SAVE_PROCESSED"; then
    mkdir -p "$SAVE_PROCESSED_DIR"
    viewer_args+=(
        --save-processed
        --save-processed-dir "$SAVE_PROCESSED_DIR"
        --save-processed-planes "$SAVE_PROCESSED_PLANES"
        --save-processed-stride "$SAVE_PROCESSED_STRIDE"
        --save-processed-max-frames "$SAVE_PROCESSED_MAX_FRAMES"
    )
    echo "Capture saving: dir=$SAVE_PROCESSED_DIR planes=$SAVE_PROCESSED_PLANES stride=$SAVE_PROCESSED_STRIDE max=$SAVE_PROCESSED_MAX_FRAMES"
fi
(
    cd "$VIEWER_DIR"
    if flag_enabled "$AUTO_PLAY"; then
        exec env DISPLAY="$DISPLAY" TOF_NET_START_MODE="$FRAME_TYPE" TOF_NET_START_FPS="$FPS" TOF_NET_GUI_AUTO_PLAY=1 stdbuf -oL -eL "$VIEWER_BIN" "${viewer_args[@]}"
    fi
    exec env DISPLAY="$DISPLAY" TOF_NET_START_MODE="$FRAME_TYPE" TOF_NET_START_FPS="$FPS" stdbuf -oL -eL "$VIEWER_BIN" "${viewer_args[@]}"
) >"$viewer_log" 2>&1 &
viewer_pid=$!

if ! wait_log "Listening for RAW ToF frames" "$viewer_log" 20; then
    die "viewer did not start listening; see $viewer_log"
fi

echo "Starting ToF collect on $TOF_TARGET..."
echo "Startup mode: $FRAME_TYPE (mode id $MODE_ID)"
echo "Startup FPS: $FPS"
collect_metadata_args=(--metadata-cache-dir "$METADATA_CACHE_DIR")
if [[ "$METADATA_CACHE" == "0" || "$METADATA_CACHE" == "false" || "$METADATA_CACHE" == "FALSE" ]]; then
    collect_metadata_args=(--no-metadata-cache)
fi
if [[ "$REFRESH_METADATA" == "1" || "$REFRESH_METADATA" == "true" || "$REFRESH_METADATA" == "TRUE" ]]; then
    collect_metadata_args+=(--refresh-metadata)
fi
if [[ "$SEND_ALL_DEALIAS" == "1" || "$SEND_ALL_DEALIAS" == "true" || "$SEND_ALL_DEALIAS" == "TRUE" ]]; then
    collect_metadata_args+=(--send-all-dealias)
fi
collect_metadata_args_q=""
for arg in "${collect_metadata_args[@]}"; do
    collect_metadata_args_q+=" $(printf '%q' "$arg")"
done
remote_now=$(date '+%Y-%m-%d %H:%M:%S')
remote_cmd=$(cat <<EOF
set -e
mkdir -p /home/analog/tof_log_all
printf '%s\n' $tof_password_q | sudo -S bash -lc 'set -o pipefail; date -s "$remote_now" >/dev/null || true; mkdir -p /home/analog/tof_log_all; cd "$TOF_COLLECT_DIR"; { if command -v stdbuf >/dev/null 2>&1; then stdbuf -oL -eL ./tof_net_collect --m "$MODE_ID" --n "$N_FRAMES" --fps "$FPS"$collect_metadata_args_q "$TOF_COLLECT_CONFIG" --server-ip "$SERVER_IP" --port "$PORT"; else ./tof_net_collect --m "$MODE_ID" --n "$N_FRAMES" --fps "$FPS"$collect_metadata_args_q "$TOF_COLLECT_CONFIG" --server-ip "$SERVER_IP" --port "$PORT"; fi; } 2>&1 | tee "$tof_log_remote"; exit \${PIPESTATUS[0]}'
EOF
)
ssh_with_password "$TOF_TARGET" "$remote_cmd" "$tof_log_local" &
ssh_collect_pid=$!

echo "Waiting for CCB/dealias metadata handshake..."
if ! wait_log "waiting for start command" "$viewer_log" "$WAIT_METADATA_SECONDS"; then
    die "metadata handshake did not complete; see $viewer_log"
fi

echo
if flag_enabled "$AUTO_PLAY"; then
    echo "Metadata ready. GUI auto Play is enabled; waiting for StartCapture."
    if ! wait_log "Sent StartCapture" "$viewer_log" 15; then
        die "GUI auto Play did not reach StartCapture; see $viewer_log"
    fi
else
    echo "Metadata ready. Click Play in the GUI now."
fi
echo "Running. Press Ctrl-C here to stop viewer and ToF collect."
echo "Viewer log: $viewer_log"
echo "ToF live log: $tof_log_local"
echo "ToF log on ToF: $TOF_TARGET:$tof_log_remote"
echo "RK USB dmesg log: $rk_usb_dmesg_log"
echo "RK USB link log: $rk_usb_link_log"
echo

while kill -0 "$viewer_pid" 2>/dev/null; do
    if [[ -n "$ssh_collect_pid" ]] && ! kill -0 "$ssh_collect_pid" 2>/dev/null; then
        echo "$(date -Is) ToF collect SSH process exited; check $tof_log_local"
        ssh_collect_pid=""
    fi
    if ! ip -brief addr | grep -Fq "$SERVER_IP/"; then
        echo "$(date -Is) warning: RK USB IP $SERVER_IP disappeared; check $rk_usb_dmesg_log"
    fi
    sleep 2
done

viewer_status=0
if ! wait "$viewer_pid"; then
    viewer_status=$?
fi
echo "$(date -Is) viewer exited with status $viewer_status; see $viewer_log"
