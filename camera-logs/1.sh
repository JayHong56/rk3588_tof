sudo date -s "2026-06-27 15:30:00"
codex resume 019f03ce-3fce-7ce0-87dd-857d227a39c6


sudo ldconfig
sudo systemctl start network-gadget.service
systemctl is-active network-gadget.service
pgrep -ax aditof-server
sudo systemctl stop network-gadget.path
sudo journalctl -u network-gadget.service -b -n 80 --no-pager

sudo bash -c '
: > /var/log/tof-live.log
printf "==== TEST BOARD_TIME=%s BOOT_ID=%s ====\n" "$(date "+%F %T %Z")" "$(cat /proc/sys/kernel/random/boot_id)" >> /var/
log/tof-live.log
journalctl -b -f -o short-precise |
while IFS= read -r line; do
printf "%s\n" "$line" >> /var/log/tof-live.log
sync -f /var/log/tof-live.log 2>/dev/null || sync
done
'

/home/analog/mark_tof_test.sh