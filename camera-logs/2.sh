## tof:
cd ~/rk3588_tof_remote/build_diag/bin
ssh -M -S /tmp/rklog-ssh -fN linaro@10.42.0.103

TS=$(date +%Y%m%d_%H%M%S)
REMOTE_LOG="/home/linaro/rk3588_tof/camera-logs/new_logs/tof_collect_${TS}.log"

./tof_net_collect.diag \
--m 0 \
config/config_crosby_adsd3500_new_modes.json \
--server-ip 10.42.0.103 \
--port 5000 \
2>&1 | tee >(ssh -S /tmp/rklog-ssh linaro@10.42.0.103 "mkdir -p /home/linaro/rk3588_tof/camera-logs/new_logs && cat >>$REMOTE_LOG")

ssh -S /tmp/rklog-ssh -O exit linaro@10.42.0.103

## rk3588:
cd /home/linaro/rk3588_tof/tof_net/build/examples/tof-net-viewer

LOG_DIR=/home/linaro/rk3588_tof/camera-logs/new_logs
TS=$(date +%Y%m%d_%H%M%S)
MARKER="$LOG_DIR/viewer_start_$TS.marker"
# touch "$MARKER"

./tof-net-viewer 2>&1 | tee "$LOG_DIR/viewer_terminal_$TS.log"