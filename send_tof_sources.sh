#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-/home/linaro/rk3588_tof}"
REMOTE="${REMOTE:-analog@10.42.0.1:/home/analog/}"
STAMP="$(date +%Y%m%d_%H%M%S)"
PKG="/tmp/tof_camera_sources_${STAMP}.tar.gz"
LIST="/tmp/tof_camera_sources_${STAMP}.list"

cd "$ROOT"

cat > "$LIST" <<'EOF'
ToF/sdcard-images-utils/nxp
ToF/apps/server/server.cpp
ToF/apps/server/CMakeLists.txt
ToF/sdk/src/connections/target/adsd3500_sensor.cpp
ToF/sdk/src/connections/target/adsd3500_sensor.h
ToF/sdk/src/connections/target
ToF/sdk
ToF/cmake
ToF/CMakeLists.txt
ToF/drivers/adsd3500/nxp
ToF-drivers/drivers/adsd3500/nxp
tof_net/sdcard-images-utils/nxp
tof_net/sdk/src/connections/target/adsd3500_sensor.cpp
tof_net/sdk/src/connections/target/adsd3500_sensor.h
tof_net/apps/server/server.cpp
EOF

if [ -d "linux-imx" ]; then
    printf '%s\n' "linux-imx" >> "$LIST"
fi

if [ -d "kernel/drivers/staging/media/imx" ] &&
   [ -f "kernel/drivers/staging/media/imx/imx8-isi-cap.c" ]; then
    cat >> "$LIST" <<'EOF'
kernel/drivers/staging/media/imx
kernel/drivers/media/i2c/adsd3500.c
kernel/drivers/media/i2c/adsd3500_regs.h
kernel/arch/arm64/boot/dts/freescale
kernel/.config
kernel/Module.symvers
kernel/modules.order
kernel/Makefile
EOF
fi

EXISTING="/tmp/tof_camera_sources_${STAMP}.existing"
MISSING="/tmp/tof_camera_sources_${STAMP}.missing"
: > "$EXISTING"
: > "$MISSING"

while IFS= read -r path; do
    [ -z "$path" ] && continue
    if [ -e "$path" ]; then
        printf '%s\n' "$path" >> "$EXISTING"
    else
        printf '%s\n' "$path" >> "$MISSING"
    fi
done < "$LIST"

echo "Packaging files from: $ROOT"
echo "Bundle: $PKG"
tar -czf "$PKG" --files-from "$EXISTING"

echo
echo "Created:"
ls -lh "$PKG"

if [ -s "$MISSING" ]; then
    echo
    echo "Missing optional paths:"
    sed 's/^/  /' "$MISSING"
fi

echo
echo "Sending to: $REMOTE"
scp -o StrictHostKeyChecking=accept-new "$PKG" "$REMOTE"

echo
echo "Done. Remote bundle name:"
basename "$PKG"
