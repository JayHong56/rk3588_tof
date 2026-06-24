#!/usr/bin/env bash

set -u
set -o pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
TOF_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
VIEWER_DIR=${TOF_VIEWER_DIR:-"$TOF_ROOT/build/examples/tof-viewer"}
APP=${TOF_GUI_APP:-"$VIEWER_DIR/ADIToFGUI"}
CAMERA_IP=${TOF_CAMERA_IP:-10.42.0.1}
OUTPUT_ROOT=${TOF_DEBUG_OUTPUT_ROOT:-"$VIEWER_DIR/debug-runs"}
SNAPSHOT_WAIT_SECONDS=${TOF_DEBUG_SNAPSHOT_WAIT_SECONDS:-120}

if [[ -n ${TOF_GUI_CONFIG:-} ]]; then
    CONFIG=$TOF_GUI_CONFIG
elif [[ -f "$VIEWER_DIR/tof-viewer_config_adsd3500_new_modes.json" ]]; then
    CONFIG="$VIEWER_DIR/tof-viewer_config_adsd3500_new_modes.json"
elif [[ -f "$VIEWER_DIR/../tof-viewer_config_adsd3500_new_modes.json" ]]; then
    CONFIG="$VIEWER_DIR/../tof-viewer_config_adsd3500_new_modes.json"
else
    CONFIG="$TOF_ROOT/examples/tof-viewer/tof-viewer_config_adsd3500_new_modes.json"
fi

usage() {
    cat <<EOF
Usage:
  $(basename "$0") run
  $(basename "$0") snapshot [PID]

run       Start ADIToFGUI under gdb. A crash automatically records all thread
          backtraces. If the GUI hangs, press Ctrl-C once in this terminal;
          gdb will stop the process and record its thread state.

snapshot  Attach to an already hung ADIToFGUI, record all thread backtraces,
          then detach without terminating it. PID is auto-detected if omitted.
          Do not use snapshot after a crash; the process no longer exists and
          run mode already captures the crash automatically.

Environment overrides:
  TOF_CAMERA_IP, TOF_GUI_APP, TOF_GUI_CONFIG, TOF_VIEWER_DIR,
  TOF_DEBUG_OUTPUT_ROOT, TOF_DEBUG_SNAPSHOT_WAIT_SECONDS
EOF
}

timestamp() {
    date '+%Y%m%d_%H%M%S'
}

new_output_dir() {
    local kind=$1
    local dir="$OUTPUT_ROOT/${kind}_$(timestamp)"
    mkdir -p "$dir"
    printf '%s\n' "$dir"
}

collect_system_info() {
    local dir=$1
    {
        echo "captured_at=$(date --iso-8601=seconds)"
        echo "hostname=$(hostname)"
        echo "kernel=$(uname -a)"
        echo "camera_ip=$CAMERA_IP"
        echo "app=$APP"
        echo "config=$CONFIG"
        echo "display=${DISPLAY:-unset}"
        echo "xauthority=${XAUTHORITY:-unset}"
        echo
        echo "== git =="
        git -C "$TOF_ROOT" rev-parse HEAD 2>&1 || true
        git -C "$TOF_ROOT" status --short 2>&1 || true
        echo
        echo "== binary =="
        file "$APP" 2>&1 || true
        ldd "$APP" 2>&1 || true
        echo
        echo "== network =="
        ip -brief address 2>&1 || true
        ip route 2>&1 || true
        echo
        echo "== processes =="
        ps -ef 2>&1 | grep -E '[A]DIToFGUI|[a]di-tof|[g]db' || true
    } >"$dir/system.txt"
}

copy_viewer_logs() {
    local dir=$1
    local marker=$2
    mkdir -p "$dir/viewer-log"
    if [[ -d "$VIEWER_DIR/log" ]]; then
        find "$VIEWER_DIR/log" -maxdepth 1 -type f -newer "$marker" \
            -exec cp -a {} "$dir/viewer-log/" \; 2>/dev/null || true
    fi
}

require_tools() {
    local missing=0
    for tool in gdb sudo; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "Missing required command: $tool" >&2
            missing=1
        fi
    done
    [[ $missing -eq 0 ]]
}

run_under_gdb() {
    require_tools || return 1
    if [[ ! -x "$APP" ]]; then
        echo "ADIToFGUI not found or not executable: $APP" >&2
        return 1
    fi
    if [[ ! -f "$CONFIG" ]]; then
        echo "Config not found: $CONFIG" >&2
        return 1
    fi

    local dir marker status
    dir=$(new_output_dir crash)
    marker="$dir/start.marker"
    touch "$marker"
    collect_system_info "$dir"

    echo "Debug output: $dir"
    echo "Use the GUI normally and click Play."
    echo "If it crashes, do not run snapshot and do not press Ctrl-C."
    echo "Wait here until 'Capture complete' is printed."
    echo "If it hangs, press Ctrl-C once here and wait for the backtrace."

    sudo -v || return 1
    cd "$VIEWER_DIR" || return 1

    set +e
    sudo -E gdb -q -batch \
        -ex 'set confirm off' \
        -ex 'set pagination off' \
        -ex 'set print thread-events off' \
        -ex 'handle SIGPIPE nostop noprint pass' \
        -ex run \
        -ex 'echo \n===== INFERIOR STATE =====\n' \
        -ex 'info program' \
        -ex 'echo \n===== ALL THREADS =====\n' \
        -ex 'info threads' \
        -ex 'thread apply all bt full' \
        -ex 'echo \n===== REGISTERS =====\n' \
        -ex 'info registers' \
        -ex detach \
        --args "$APP" --ip "$CAMERA_IP" "$CONFIG" \
        2>&1 | tee "$dir/gdb.txt"
    status=${PIPESTATUS[0]}
    set -e

    printf '%s\n' "$status" >"$dir/gdb-exit-status.txt"
    copy_viewer_logs "$dir" "$marker"
    collect_system_info "$dir"
    echo "Capture complete: $dir"
    return "$status"
}

snapshot_hung_process() {
    require_tools || return 1
    local pid=${1:-}
    sudo -v || return 1
    if [[ -z "$pid" ]]; then
        # ADIToFGUI normally runs as root. Use sudo both to see it reliably
        # and to avoid treating EPERM from kill(2) as "process not found".
        pid=$(sudo pgrep -n -f "^${APP//./\\.}( |$)" || true)
    fi
    if [[ -z "$pid" ]] || ! sudo kill -0 "$pid" 2>/dev/null; then
        echo "No running ADIToFGUI process found." >&2
        echo "snapshot is only for a process that is still alive but hung." >&2
        echo "For a crash, use '$(basename "$0") run' and wait for 'Capture complete'." >&2
        return 1
    fi

    local tracer_pid
    tracer_pid=$(sudo awk '/^TracerPid:/ { print $2 }' "/proc/$pid/status" 2>/dev/null || true)
    if [[ -n "$tracer_pid" && "$tracer_pid" != 0 ]]; then
        local tracer_cmd run_dir deadline finalize_deadline capture_status
        tracer_cmd=$(sudo cat "/proc/$tracer_pid/cmdline" 2>/dev/null | tr '\0' ' ' || true)
        if [[ "$tracer_cmd" == *gdb* ]]; then
            run_dir=$(find "$OUTPUT_ROOT" -mindepth 1 -maxdepth 1 -type d \
                -name 'crash_*' -printf '%T@ %p\n' 2>/dev/null \
                | sort -nr | sed -n '1s/^[^ ]* //p')
            echo "PID $pid is already controlled by gdb PID $tracer_pid."
            echo "Triggering the existing run-mode gdb to capture all threads."
            if ! sudo kill -INT "$tracer_pid"; then
                echo "Failed to interrupt gdb PID $tracer_pid." >&2
                return 1
            fi

            echo "Waiting here for the capture to complete (up to ${SNAPSHOT_WAIT_SECONDS}s)."
            deadline=$((SECONDS + SNAPSHOT_WAIT_SECONDS))
            while sudo kill -0 "$tracer_pid" 2>/dev/null; do
                if ((SECONDS >= deadline)); then
                    echo "Timed out waiting for gdb PID $tracer_pid to finish capture." >&2
                    if [[ -n "$run_dir" ]]; then
                        echo "Partial debug output: $run_dir" >&2
                    fi
                    return 1
                fi
                sleep 1
            done

            if [[ -z "$run_dir" ]]; then
                echo "Capture finished, but its debug output directory could not be located."
                return 0
            fi

            # Give the run-mode wrapper a short window to finish copying logs
            # and writing the gdb status after the debugger itself exits.
            finalize_deadline=$((SECONDS + 10))
            while [[ ! -f "$run_dir/gdb-exit-status.txt" ]] \
                && ((SECONDS < finalize_deadline)); do
                sleep 1
            done

            echo "Capture complete: $run_dir"
            if [[ ! -f "$run_dir/gdb-exit-status.txt" ]]; then
                if grep -q '^\[Inferior .* detached\]$' "$run_dir/gdb.txt" 2>/dev/null; then
                    echo "Note: gdb completed, but the original run wrapper did not finalize its metadata."
                    return 0
                fi
                echo "gdb exited without a complete capture log." >&2
                return 1
            fi

            capture_status=$(<"$run_dir/gdb-exit-status.txt")
            if [[ "$capture_status" =~ ^[0-9]+$ ]]; then
                return "$capture_status"
            fi
            echo "Invalid gdb exit status in $run_dir/gdb-exit-status.txt" >&2
            return 1
        fi
        echo "PID $pid is already traced by PID $tracer_pid: $tracer_cmd" >&2
        echo "Refusing to attach a second debugger." >&2
        return 1
    fi

    local dir
    dir=$(new_output_dir hang)
    collect_system_info "$dir"
    ps -L -p "$pid" -o pid,tid,stat,pcpu,pmem,wchan:40,comm \
        >"$dir/threads-before.txt" 2>&1 || true
    cp "/proc/$pid/status" "$dir/proc-status.txt" 2>/dev/null || true
    cp "/proc/$pid/maps" "$dir/proc-maps.txt" 2>/dev/null || true

    echo "Capturing PID $pid; the GUI will pause briefly and then resume."
    set +e
    sudo gdb -q -batch -p "$pid" \
        -ex 'set confirm off' \
        -ex 'set pagination off' \
        -ex 'echo ===== INFERIOR STATE =====\n' \
        -ex 'info program' \
        -ex 'echo \n===== ALL THREADS =====\n' \
        -ex 'info threads' \
        -ex 'thread apply all bt full' \
        -ex 'echo \n===== REGISTERS =====\n' \
        -ex 'info registers' \
        -ex detach \
        2>&1 | tee "$dir/gdb-attach.txt"
    local status=${PIPESTATUS[0]}
    set -e

    ps -L -p "$pid" -o pid,tid,stat,pcpu,pmem,wchan:40,comm \
        >"$dir/threads-after.txt" 2>&1 || true
    echo "Snapshot complete: $dir"
    return "$status"
}

case ${1:-run} in
run)
    run_under_gdb
    ;;
snapshot)
    snapshot_hung_process "${2:-}"
    ;;
-h|--help|help)
    usage
    ;;
*)
    usage >&2
    exit 2
    ;;
esac
