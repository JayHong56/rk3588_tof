## tof:
sudo ldconfig
sudo systemctl start network-gadget.service
# systemctl is-active network-gadget.service
pgrep -ax aditof-server
sudo systemctl stop network-gadget.path
sudo journalctl -u network-gadget.service -b -n 80 --no-pager

cd ~/rk3588_tof_remote/tof_net/build/examples/tof-net-collect

TS=$(date +%Y%m%d_%H%M%S)
LOCAL_LOG_DIR="/home/analog/tof_log_all/live"
LOCAL_LOG="$LOCAL_LOG_DIR/tof_collect_${TS}.log"
mkdir -p "$LOCAL_LOG_DIR"

export LD_LIBRARY_PATH="$PWD/../../sdk:$PWD/../../sdk/common/adi/depth-compute-opensource:/opt/glog/lib:/opt/websockets/lib:$LD_LIBRARY_PATH"

sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" stdbuf -oL -eL ./tof_net_collect \
--m 0 \
config/config_crosby_adsd3500_new_modes.json \
--server-ip 10.42.0.103 \
--port 5000 \
2>&1 | while IFS= read -r line; do
    printf '%s\n' "$line"
    printf '%s\n' "$line" >> "$LOCAL_LOG"
    sync -f "$LOCAL_LOG" 2>/dev/null || sync
done

ls -lt /home/analog/tof_log_all/live/
tail -n 120 /home/analog/tof_log_all/live/tof_collect_*.log

################################################################################
################################################################################
################################################################################
## rk3588:

cd /home/linaro/rk3588_tof/tof_net/build/examples/tof-net-viewer

LOG_DIR=/home/linaro/rk3588_tof/camera-logs/new_logs
TS=$(date +%Y%m%d_%H%M%S)
MARKER="$LOG_DIR/viewer_start_$TS.marker"
# touch "$MARKER"

./tof-net-viewer 2>&1 | tee "$LOG_DIR/viewer_terminal_$TS.log"