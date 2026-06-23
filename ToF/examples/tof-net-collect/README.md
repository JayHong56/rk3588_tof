# tof_net_collect

`tof_net_collect` is the Machine A acquisition process. It is intentionally based on the acquisition flow used by `examples/data_collect` in ToF rel-4.2.1.

It preserves the `data_collect` command style:

```bash
./tof_net_collect   --ip 10.42.0.1   --m 0   config/config_crosby_adsd3500_new_modes.json   --server-ip 192.168.3.32   --bind-ip 192.168.3.146   --port 5000
```

The first positional `FILE` is the same ADI camera initialization JSON used by `data_collect`.

## Important behavior

Machine A does not compute depth. The program does the following:

1. Enumerates the camera with the same `--ip` / `getCameraListAtIp()` style as `data_collect`.
2. Applies `setControl("initialization_config", FILE)`.
3. Resolves `--m` using `getFrameTypeNameFromId()`.
4. Calls `setControl("enableDepthCompute", "off")`.
5. Sets the camera frame type.
6. Waits for `tof_net_viewer` to send `StartCapture`.
7. Calls `camera->requestFrame(&frame)` and extracts `frame.getData("raw", ...)`.
8. Compresses the raw buffer with Zstd.
9. Sends the compressed raw frame to Machine B.

No depth, AB, confidence, or XYZ computation is done on Machine A.

## Options inherited from data_collect

Accepted options include:

```text
--ip
--m
--ft
--fw
--fps
--ccb
--ext_fsync
--wt
--n
FILE
```

`--ft` is accepted for compatibility but is forced to `raw` because this split design requires Machine B to compute depth.

## Network options added by tof_net_collect

```text
--server-ip <B_IP>
--bind-ip <A_IP>
--port <PORT>
--zstd-level <N>
--no-reconnect
```

./tof_net_collect \
  --ip 10.42.0.1 \
  --m 0 \
  config/config_crosby_adsd3500_new_modes.json \
  --server-ip 192.168.3.32 \
  --bind-ip 192.168.3.146 \
  --port 5000
## Live CCB + Dealias Data transfer to tof-net-viewer

After each TCP connection to Machine B, `tof_net_collect`:

1. Exports the current module CCB via `saveModuleCCB` → `CcbFile` message.
2. **Exports per-mode intrinsics + dealias parameters** directly from ADSD3500
   hardware (cmd 0x01/0x02) → one `DealiasData` message per available frame type.
3. Waits for `StartCapture`.

> **Note:** CFG is not exported. ADSD3500 does not store CFG in EEPROM and the
> ISP TOFI path (`InitTofiConfig_isp`) does not use it.

Expected log on Machine A:

```text
Exported current module CCB: /tmp/tof_net_collect_module_<pid>.ccb (<bytes> bytes)
sent current module CCB to Machine B: <bytes> bytes from /tmp/tof_net_collect_module_<pid>.ccb
Exported dealias data for sr-native (mode=0, 120 bytes)
Exported dealias data for lr-native (mode=1, 120 bytes)
sent dealias data for sr-native (120 bytes)
...
```
