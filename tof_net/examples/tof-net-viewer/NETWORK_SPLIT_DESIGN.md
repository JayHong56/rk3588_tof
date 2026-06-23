# ADI ToF SDK 网络拆分系统设计文档

## 概述

将 ADI ToF SDK 的原始 `tof-viewer`（采集+计算+显示一体）拆分为局域网分离架构：

- **Machine A**（采集端 `tof-net-collect`）：直连 ToF 摄像头，仅采集 RAW 数据并压缩传输
- **Machine B**（显示端 `tof-net-viewer`）：接收 RAW 数据，本地完成 TOFI 深度计算并 GUI 渲染
- **共享层**（`tof-net-common`）：TCP 协议定义、Socket 封装、Zstd 压缩

```
┌─ Machine A ─────────────────────────┐      ┌─ Machine B ──────────────────────┐
│ tof-net-collect                      │      │ tof-net-viewer                   │
│                                      │ TCP  │                                  │
│ Camera ADSD3500                      │─────▶│ handleCcbFile() → 保存 CCB       │
│ enableDepthCompute = off             │      │ handleDealiasData() → 内参+消畸变 │
│                                      │      │                                  │
│ export_module_ccb() → CcbFile ───────┼─────▶│ ensureTofiIsp()                  │
│ export_dealias_data() → DealiasData ─┼─────▶│   InitTofiConfig_isp()           │
│                                      │      │   InitTofiCompute()              │
│ get_frame("raw") → Zstd → DataFrame ─┼─────▶│   TofiCompute() → depth/ir/xyz   │
│                                      │      │                                  │
│ switchMode() ← StartCapture ─────────┼──────│ GUI: ImGui + OpenGL              │
└──────────────────────────────────────┘      └──────────────────────────────────┘
```

---

## 项目结构

```
examples/
├── tof-net-common/          # 共享网络层
│   ├── include/tof_net/
│   │   ├── protocol.hpp     # 消息类型、Payload 结构体
│   │   ├── socket.hpp       # TCP Socket (connect/listen/send/recv)
│   │   └── zstd_codec.hpp   # Zstd 压缩/解压
│   └── src/
│       ├── protocol.cpp
│       ├── socket.cpp
│       └── zstd_codec.cpp
│
├── tof-net-collect/         # Machine A 采集端
│   ├── src/main.cpp         # CameraRawSource + session()
│   ├── include/
│   ├── CMakeLists.txt
│   └── README.md
│
├── tof-net-viewer/          # Machine B 显示端
│   ├── src/
│   │   ├── ADINetworkToFStream.cpp  # 网络 TOFI 计算核心
│   │   ├── ADIMainWindow.cpp        # GUI 主窗口
│   │   ├── ADIController.cpp        # 控制器（网络模式）
│   │   └── ...
│   ├── include/
│   │   ├── ADINetworkToFStream.h
│   │   ├── ADIMainWindow.h
│   │   └── ...
│   ├── tof-viewer_config.json       # 运行配置
│   └── CMakeLists.txt
│
└── tof-viewer/              # 原始本地版（参照基准，未修改）
```

---

## 协议设计

### 消息类型

| 类型 | 值 | 方向 | 说明 |
|---|---|---|---|
| `Hello` | 1 | A→B | 握手，携带 app name + SDK 版本 |
| `StartCapture` | 2 | B→A | 开始采集，携带 frame_type + mode |
| `StopCapture` | 3 | B→A | 停止采集 |
| `DataFrame` | 4 | A→B | RAW 帧数据（Zstd 压缩） |
| `Status` | 5 | A→B | 状态报告 |
| `Error` | 6 | A→B | 错误报告 |
| `Shutdown` | 7 | A→B | 断开连接 |
| `CcbFile` | 8 | A→B | 模块 CCB 校准数据 (243KB) |
| `CfgFile` | 9 | - | 已废弃（ADSD3500 无 CFG） |
| `DealiasData` | 10 | A→B | 每模式内参+消畸变参数 (88B/mode) |

### 消息格式

```
┌──────────────────────────────────────┐
│ MessageHeader (20 bytes)             │
│  magic=0x31525441 version=1          │
│  type header_bytes=20 payload_bytes  │
├──────────────────────────────────────┤
│ Payload (variable)                   │
│  depends on MessageType              │
└──────────────────────────────────────┘
```

### 连接流程

```
Machine A                          Machine B
  │                                   │
  │──── TCP connect ────────────────▶│ (listening on 0.0.0.0:5000)
  │──── Hello ──────────────────────▶│
  │──── CcbFile (CCB 243KB) ────────▶│ handleCcbFile() → 保存 CCB
  │──── DealiasData × 7 modes ──────▶│ handleDealiasData() → 存内参+消畸变
  │──── Status("waiting") ──────────▶│
  │                                   │ ← user clicks Play
  │◀─── StartCapture(frame_type) ────│
  │     switchMode() → start()        │
  │──── DataFrame (Zstd RAW) ────────▶│ TofiCompute → depth/ir → GUI
  │──── DataFrame × N ───────────────▶│ ...
  │                                   │ ← user clicks Stop / mode change
  │◀─── StopCapture ─────────────────│
  │◀─── StartCapture(new_ft) ────────│ switchMode() → restart
  │──── DataFrame (new mode RAW) ────▶│ rebuild TOFI context → GUI
```

---

## 核心数据流

### 1. 校准数据（连接时传输一次）

```
采集端:
  export_module_ccb()
    → camera->setControl("saveModuleCCB", path)
    → SDK 内部 readAdsd3500CCB() 从硬件 cmd 0x13 读取
    → 读取文件 → TCP CcbFile 消息

  export_dealias_data()
    → camera->getAvailableFrameTypes()
    → 对每个 frame_type:
        sensor->adsd3500_read_payload_cmd(0x01, intrinsics, 56)  // 内参
        sensor->adsd3500_read_payload_cmd(0x02, dealiasParams, 32) // 消畸变
        → 组装 TofiXYZDealiasData
    → TCP DealiasData ×7 消息 (sr-native, lr-native, sr-qnative,
      lr-qnative, pcm-native, sr-mixed, lr-mixed)

显示端:
  handleCcbFile()    → 保存 CCB 到 ./config/received_machine_a_module.ccb
  handleDealiasData() → convertModeName(ft) → m_xyzDealiasData[modeId]
  注: pcm-native 的 dealias 数据为 rows=0 cols=0（被动红外模式，正常）
```

### 2. 帧数据（运行时每帧传输）

```
采集端:
  camera->requestFrame(&frame)
  frame.getData("raw", &pData)         // uint16* 原始数据
  zstd_compress(raw, level=1)          // 压缩（典型 ~3:1）
  DataFrame 消息:
    header: frame_id, raw_width, raw_height, raw_bytes,
            compressed_bytes, codec=Zstd, mode, frame_type
    payload: Zstd 压缩的 RAW 数据

显示端:
  zstd_decompress → rawBytes
  rawWords = rawBytes / sizeof(uint16_t)
  ensureTofi() → ensureTofiIsp()       // 按需初始化/复用 TOFI 上下文
  TofiCompute(rawWords, tofiContext)   // Raw → Depth/IR/XYZ
  复制到 aditof::Frame → GUI 渲染
```

### 3. 模式切换（运行时动态切换）

```
显示端 GUI 选择新模式 → StartCapture(frame_type=lr-qnative)
采集端 switchMode("lr-qnative")
  → camera->stop()
  → camera->setFrameType("lr-qnative")
  → camera->start()
  → 后续 DataFrame 携带新 frame_type
显示端 ensureTofiIsp() 检测 mode 变化
  → convertModeName() 查表 → 新 convertedMode
  → m_xyzDealiasData[newMode] 已就绪（连接时接收）
  → releaseTofi() → InitTofiConfig_isp + InitTofiCompute 重建上下文
```

---

## ADSD3500 ISP 路径对齐

`tof-net-viewer` 的 `ensureTofiIsp()` 与 SDK 内部 `CameraItof::initComputeLibrary()` 完全对齐：

| 步骤 | SDK 内部 | tof-net-viewer |
|---|---|---|
| Mode 转换 | `ModeInfo::convertCameraMode()` | `convertModeName()` 自建查表 |
| 内参+消畸变 | 硬件 cmd 0x01/0x02 直接读取 | 采集端读取后 TCP 传输 |
| TOFI 配置 | `InitTofiConfig_isp(depthIni, mode, status, xyzDealias)` | 完全相同 |
| TOFI Compute Init | `InitTofiCompute(p_tofi_cal_config, status)` | 完全相同 |
| Compute | `TofiCompute(raw, ctx, nullptr)` | 完全相同 |

### opensource 版适配

使用 `USE_DEPTH_COMPUTE_OPENSOURCE=ON` 时，`InitTofiCompute` 和 `InitTofiConfig_isp` 来自开源实现：

| 问题 | 说明 | 解决方案 |
|---|---|---|
| INI 指针生命周期 | `InitTofiConfig_isp` 内部 `new ConfigFileData` 保存数据指针，调完即删会导致悬空 | `m_ispIniCopy` 成员变量保活 INI 数据 |
| 输出缓冲区未分配 | opensource `InitTofiCompute` 将所有输出指针 `p_depth/ab/conf/xyz_frame` 置零 | 手动 `new[]` 分配，`releaseTofi()` 中 `delete[]` |
| `n_rows`/`n_cols` 未设 | opensource `InitTofiCompute` 不设置 | 从 dealias 数据手动赋值 |
| CFG 不存在 | ADSD3500 EEPROM 无 CFG，`saveModuleCFG` 返回 UNAVAILABLE | ISP 路径不需要 CFG，采集端不发送 |
| `GetXYZ_DealiasData` 不适用 | 对 ADSD3500 的 CCB 返回全零 | 采集端从硬件 cmd 0x01/0x02 直接读取后发送 |

---

## 构建与运行

### 编译

```bash
mkdir build && cd build
cmake .. \
    -DWITH_EXAMPLES=ON \
    -DWITH_NETWORK=ON \
    -DUSE_DEPTH_COMPUTE_OPENSOURCE=ON \
    -DUSE_DEPTH_COMPUTE_STUBS=OFF \
    -DCMAKE_PREFIX_PATH="/opt/glog;/opt/protobuf;/opt/websockets"
make -j4
```

### 快速构建函数（Debian/ARM）

```bash
rbtof() {
    sudo rm -rf /home/linaro/rk3588_tof/ToF/build
    cd /home/linaro/rk3588_tof/ToF || return
    mkdir build && cd build || return
    cmake .. \
        -DWITH_EXAMPLES=ON -DWITH_NETWORK=ON \
        -DUSE_DEPTH_COMPUTE_OPENSOURCE=ON -DUSE_DEPTH_COMPUTE_STUBS=OFF \
        -DCMAKE_PREFIX_PATH="/opt/glog;/opt/protobuf;/opt/websockets" \
        2>&1 | tee cmake.log
    if [ ${PIPESTATUS[0]} -ne 0 ]; then
        echo "CMake failed:"; tail -50 cmake.log; return 1
    fi
    make -j4 2>&1 | tee build.log
    if [ ${PIPESTATUS[0]} -ne 0 ]; then
        echo "Build failed:"; grep -E "error:" build.log | sort -u | head -30
        return 1
    fi
    echo "Build success!"
}
```

### 运行（同一台机器）

```bash
# 终端 1：显示端先启动
cd build/examples/tof-net-viewer
./tof-net-viewer

# 终端 2：采集端
cd build/examples/tof-net-collect
./tof_net_collect --ip 10.42.0.1 --m 0 \
    config/config_crosby_adsd3500_new_modes.json \
    --server-ip 127.0.0.1 --port 5000
```

### 配置文件

`tof-viewer_config.json`:
```json
{
    "net_listen_ip": "0.0.0.0",
    "net_listen_port": "5000",
    "net_is_adsd3500": "true",
    "modes": "lr-qnative,sr-qnative,lr-native,sr-native,lr-mixed,sr-mixed,pcm-native",
    "DEPTH_INI": "./config/RawToDepthAdsd3500_lr-qnative.ini;./config/RawToDepthAdsd3500_lr-native.ini;./config/RawToDepthAdsd3500_sr-qnative.ini;./config/RawToDepthAdsd3500_sr-native.ini;./config/RawToDepthAdsd3500_sr-mixed.ini;./config/RawToDepthAdsd3500_lr-mixed.ini;./config/RawToDepthAdsd_pcm-native.ini",
    "net_default_mode": "0",
    "net_received_ccb": "./config/received_machine_a_module.ccb"
}
```

---

## 命令行选项

### tof-net-collect

| 选项 | 说明 | 默认值 |
|---|---|---|
| `FILE` (位置) | ADI 相机初始化 JSON | 必填 |
| `--ip` | 摄像头 IP | 本地枚举 |
| `--m` | 初始模式 ID | 0 (= sr-native) |
| `--server-ip` | Machine B IP | 127.0.0.1 |
| `--bind-ip` | 本机绑定 IP | 自动 |
| `--port` | TCP 端口 | 5000 |
| `--zstd-level` | 压缩级别 (1-22) | 1 |
| `--n` | 最大帧数 (0=无限) | 0 |
| `--fw` | 固件升级路径 | - |
| `--ccb` | 保存 CCB 到文件 | - |
| `--wt` | 预热秒数 | 0 |
| `--ext_fsync` | 同步模式 | 0 |
| `--no-reconnect` | 断连不重试 | false |

---

## 相关 SDK 源码引用

| 组件 | 文件 | 关键行 |
|---|---|---|
| `InitTofiConfig_isp` (API) | `sdk/common/adi/tofi/tofi_config.h` | L82 |
| `TofiCompute` (API) | `sdk/common/adi/tofi/tofi_compute.h` | L68 |
| `FreeTofiConfig` / `FreeTofiCompute` | 同上 | L90+ |
| `CameraItof::initComputeLibrary` | `sdk/src/cameras/itof-camera/camera_itof.cpp` | L921 |
| `CameraItof::readAdsd3500CCB` | 同上 | L1512 |
| `CameraItof::loadModuleData` | 同上 | L1261 |
| `ModeInfo::g_newModesAdsd3500` | `sdk/src/cameras/itof-camera/mode_info.cpp` | L36 |
| `ModeInfo::convertCameraMode` | 同上 | L98 |
| Opensource `InitTofiConfig_isp` | `sdk/common/adi/depth-compute-opensource/src/tofiConfig.cpp` | L50 |
| Opensource `InitTofiCompute` | `sdk/common/adi/depth-compute-opensource/src/tofiCompute.cpp` | L170 |
| Opensource `TofiCompute` | 同上 | L260 |
| Opensource `DeInterleaveDepth` | 同上 | L219 |
