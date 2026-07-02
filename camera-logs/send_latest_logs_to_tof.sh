#!/usr/bin/env bash
set -euo pipefail

LOG_DIR="/home/linaro/rk3588_tof/camera-logs/new_logs"
REMOTE="analog@10.42.0.1"
REMOTE_DIR="tof_log_all/viewer"
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage:
  camera-logs/send_latest_logs_to_tof.sh [options]

Options:
  --log-dir DIR       Local log directory.
                      Default: /home/linaro/rk3588_tof/camera-logs/new_logs
  --remote USER@HOST  Destination ssh target. Default: analog@10.42.0.1
  --remote-dir DIR    Destination directory on remote host, relative to home
                      unless absolute. Default: tof_log_all/viewer
  --dry-run           Show which viewer terminal log would be sent.
  -h, --help          Show this help.
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --log-dir)
            [[ $# -ge 2 ]] || die "--log-dir requires a value"
            LOG_DIR="$2"
            shift 2
            ;;
        --remote)
            [[ $# -ge 2 ]] || die "--remote requires a value"
            REMOTE="$2"
            shift 2
            ;;
        --remote-dir)
            [[ $# -ge 2 ]] || die "--remote-dir requires a value"
            REMOTE_DIR="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown argument: $1"
            ;;
    esac
done

[[ -d "$LOG_DIR" ]] || die "log directory does not exist: $LOG_DIR"

latest_file() {
    local pattern="$1"

    find "$LOG_DIR" -maxdepth 1 -type f -name "$pattern" -printf '%f\n' |
        sort |
        tail -n 1
}

viewer_file="$(
    latest_file "viewer_terminal_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_[0-9][0-9][0-9][0-9][0-9][0-9].log"
)"

[[ -n "$viewer_file" ]] || die "no viewer_terminal logs found in $LOG_DIR"

echo "Viewer log: $viewer_file"
echo "Remote:  $REMOTE:$REMOTE_DIR/"

if ((DRY_RUN)); then
    echo "Dry run only; nothing sent."
    exit 0
fi

remote_dir_quoted="$(printf '%q' "$REMOTE_DIR")"
ssh "$REMOTE" "mkdir -p $remote_dir_quoted"
scp "$LOG_DIR/$viewer_file" "$REMOTE:$REMOTE_DIR/"

echo "Sent: $REMOTE:$REMOTE_DIR/$viewer_file"