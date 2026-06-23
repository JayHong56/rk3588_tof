# 双机网络化ToF系统

**User:** Anonymous  
**Created:**   
**Updated:**   
**Exported:** 6/13/2026 14:10:11  
**Link:** [https://chatgpt.com/share/6a1e6b03-9f78-8329-9465-84fc705bc26f](https://chatgpt.com/share/6a1e6b03-9f78-8329-9465-84fc705bc26f)  

## Response:
我会按你确认的目标，把方案收敛成可落地的工程设计：先明确数据边界，再给出模块划分、协议、构建顺序和关键验证点。

已思考 23s

确认，后续工程目标就按这个版本收敛：

```
ToF rel-4.2.1
双机 A/B 架构

机器 A：
  连接 ToF 相机
  采集 raw frame
  附带 metadata / mode / frame type / calibration 信息
  Zstd 压缩
  TCP 发送给机器 B

机器 B：
  TCP 接收
  解压 raw frame
  调用 TOFI / RawToDepth 计算 depth / ir / conf / xyz
  接入 tof-viewer GUI 显示
```

一、推荐的最终工程结构
-----------

建议不要直接污染原始 `tof-viewer`，而是在 `ToF rel-4.2.1` 内新增网络模块：

```
ToF/
├── apps/
│   ├── tof-viewer/                  # 原始 GUI，可复用显示逻辑
│   ├── tof-net-collect/             # 机器 A：采集 raw + 发送
│   │   ├── CMakeLists.txt
│   │   └── main.cpp
│   └── tof-net-viewer/              # 机器 B：接收 raw + TOFI + GUI
│       ├── CMakeLists.txt
│       ├── main.cpp
│       ├── NetTofDevice.cpp
│       ├── NetTofDevice.hpp
│       ├── RawToDepthWorker.cpp
│       └── RawToDepthWorker.hpp
│
├── tof-net-common/
│   ├── CMakeLists.txt
│   ├── tof_net_protocol.hpp
│   ├── tcp_socket.hpp
│   ├── tcp_socket.cpp
│   ├── zstd_codec.hpp
│   ├── zstd_codec.cpp
│   ├── crc32.hpp
│   └── crc32.cpp
│
└── config/
    ├── RawToDepthAdsd3500_lrqmp.ini
    ├── RawToDepthAdsd3500_lrmp.ini
    ├── RawToDepthAdsd3500_srqmp.ini
    ├── RawToDepthAdsd3500_srmp.ini
    └── tof-viewer_config.json
```

ADI 文档里 SDK 的高层调用链本身就是 `System -> Camera -> Frame`，通过 `requestFrame()` 获取帧，`Frame` 负责保存数据及分辨率等信息；所以 A 端应该基于这个路径取 raw，而不是绕过 SDK 直接读底层设备。[![](https://www.google.com/s2/favicons?domain=https://analogdevicesinc.github.io&sz=128)analogdevicesinc.github.io](https://analogdevicesinc.github.io/aditof_sdk/)

二、A 端必须发送什么
-----------

A 端不能只发送 raw 二进制。必须发送：

```
1. raw frame 数据
2. metadata 数据
3. frameType，例如 lr-qnative / sr-qnative / lr-native / sr-native
4. mode，例如 lrqmp / srqmp / lrmp / srmp
5. raw width / raw height
6. output depth width / output depth height
7. pixel type，例如 uint16
8. frame id
9. timestamp
10. 当前使用的 RawToDepth ini 名称
11. module calibration / CCB / loadModuleData 后的关键状态
```

原因是 B 端要做 TOFI 深度计算，必须知道 raw frame 与哪一套深度计算参数对应。ADI 示例配置中 `initialization_config` 会指定多个 `DEPTH_INI` 文件，例如 `RawToDepthAdsd3500_lrqmp.ini`、`RawToDepthAdsd3500_lrmp.ini` 等；这说明 raw-to-depth 不是纯算法输入，还强依赖模式配置文件。[![](https://www.google.com/s2/favicons?domain=https://wiki.analog.com&sz=128)模拟器件百科](https://wiki.analog.com/resources/eval/user-guides/eval-adsd3100-nxz-development)

ADI wiki 也明确示例流程包含 `setFrameType()`、`start()`、`requestFrame()`、`frame.getData()`、`frame.getDataDetails()`，因此 A 端采集逻辑应直接沿用这条链路。[![](https://www.google.com/s2/favicons?domain=https://wiki.analog.com&sz=128)模拟器件百科](https://wiki.analog.com/resources/eval/user-guides/eval-adsd3100-nxz-development)

三、建议协议头
-------

建议定义固定二进制协议，不要发 JSON 大包，也不要裸发压缩数据。

```
#pragma pack(push, 1)
struct TofNetFrameHeader {
    uint32_t magic;              // 'TOFN'
    uint16_t version;            // 1
    uint16_t headerSize;

    uint64_t frameId;
    uint64_t timestampNs;

    uint32_t rawWidth;
    uint32_t rawHeight;
    uint32_t rawStrideBytes;
    uint32_t rawPixelBytes;      // usually 2

    uint32_t depthWidth;
    uint32_t depthHeight;

    uint32_t frameTypeLen;
    uint32_t modeLen;
    uint32_t iniNameLen;

    uint32_t metadataRawBytes;
    uint32_t metadataCompressedBytes;

    uint32_t rawRawBytes;
    uint32_t rawCompressedBytes;

    uint32_t codec;              // 0 = none, 1 = zstd
    uint32_t flags;              // bit0: has_metadata, bit1: little_endian

    uint32_t headerCrc32;
    uint32_t payloadCrc32;
};
#pragma pack(pop)
```

payload 顺序固定：

```
[TofNetFrameHeader]
[frameType string]
[mode string]
[iniName string]
[compressed metadata]
[compressed raw]
```

这样 B 端可以严格校验：

```
magic 是否正确
version 是否匹配
raw size 是否等于 width × height × pixelBytes
zstd 解压后大小是否等于 rawRawBytes
crc32 是否正确
frameType / mode / ini 是否与本地 TOFI 配置匹配
```

四、A 端采集逻辑
---------

A 端主流程建议如下：

```
aditof::System system;
std::vector<std::shared_ptr<aditof::Camera>> cameras;

system.getCameraList(cameras);
auto camera = cameras.front();

camera->setControl("initialization_config", configFile);
camera->initialize();

camera->setControl("loadModuleData", "call");

camera->setFrameType("lr-qnative");
camera->start();

uint64_t frameId = 0;

while (running) {
    aditof::Frame frame;
    camera->requestFrame(&frame);

    uint16_t *raw = nullptr;
    frame.getData("raw", &raw);

    aditof::FrameDataDetails rawDetails;
    frame.getDataDetails("raw", rawDetails);

    uint16_t *metadata = nullptr;
    frame.getData("metadata", &metadata);

    aditof::FrameDataDetails metaDetails;
    frame.getDataDetails("metadata", metaDetails);

    compress_and_send(raw, rawDetails, metadata, metaDetails, frameId++);
}

camera->stop();
```

ADI 文档中的帧类型里，`sr-native` / `lr-native` 的 raw 是 `1024 × 4096`，而 `sr-qnative` / `lr-qnative` 的 raw 是 `2560 × 512`；对应输出 depth / ir / conf / xyz 分辨率可能是 `1024 × 1024` 或 `512 × 512`，所以协议里必须同时记录 raw 分辨率和输出分辨率。[![](https://www.google.com/s2/favicons?domain=https://wiki.analog.com&sz=128)模拟器件百科](https://wiki.analog.com/resources/eval/user-guides/eval-adsd3100-nxz-development)

五、B 端处理逻辑
---------

B 端不要一开始就直接改 GUI。建议分三层：

```
第一层：tof-net-recv
  只负责接收、解压、校验、保存 raw.bin / metadata.bin

第二层：tof-net-tofi-test
  读取 raw.bin / metadata.bin
  调用 TOFI / RawToDepth
  生成 depth.bin / ir.bin / conf.bin / xyz.bin
  与 A 端本地 SDK 输出做对比

第三层：tof-net-viewer
  把 TOFI 输出接入 tof-viewer 的显示管线
```

B 端核心流程：

```
TCP accept
接收 header
接收 payload
校验 magic / crc / size
Zstd 解压 raw
Zstd 解压 metadata
根据 frameType / mode / ini 选择 RawToDepth 配置
调用 TOFI compute
得到 depth / ir / conf / xyz
推入 GUI frame queue
tof-viewer 刷新显示
```

六、最关键的验证顺序
----------

这个项目不要先做 GUI，应该按下面顺序验证：

### 阶段 1：单机 raw 对齐

在机器 A 上同时保存：

```
raw_A.bin
metadata_A.bin
depth_A.bin
ir_A.bin
conf_A.bin
xyz_A.bin
```

其中 `raw_A.bin` 是从 `frame.getData("raw")` 得到，`depth_A.bin` 是同一帧从 SDK 直接得到。

目的：确认同一帧 raw 与 SDK 输出一一对应。

### 阶段 2：B 端离线 TOFI

把 A 端保存的：

```
raw_A.bin
metadata_A.bin
tof-viewer_config.json
RawToDepth*.ini
module data / calibration
```

复制到 B 端，离线跑：

```
./tof-net-tofi-test \
  --raw raw_A.bin \
  --metadata metadata_A.bin \
  --frame-type lr-qnative \
  --mode lrqmp \
  --config config/tof-viewer_config.json \
  --out out/
```

输出：

```
out/depth_B.bin
out/ir_B.bin
out/conf_B.bin
out/xyz_B.bin
```

然后比较：

```
cmp depth_A.bin out/depth_B.bin
cmp ir_A.bin out/ir_B.bin
```

如果不能 bit-exact，也至少要统计：

```
mean abs error
max abs error
valid pixel ratio
depth histogram
```

### 阶段 3：网络传输

A 端：

```
./tof-net-collect \
  --server 192.168.1.20 \
  --port 5000 \
  --config config/tof-viewer_config.json \
  --frame-type lr-qnative \
  --mode lrqmp \
  --fps 10 \
  --codec zstd
```

B 端：

```
./tof-net-viewer \
  --listen 0.0.0.0 \
  --port 5000 \
  --config config/tof-viewer_config.json \
  --compute tofi \
  --show depth,ir,conf,pointcloud
```

### 阶段 4：GUI 接入

最后才把 B 端输出接入 `tof-viewer` 的显示层。

七、带宽判断
------

如果 A 端发送 `lr-qnative` raw：

```
raw = 2560 × 512 × 2 bytes ≈ 2.5 MB/frame
10 fps ≈ 25 MB/s
30 fps ≈ 75 MB/s
```

1GbE 理论上可以承受，但考虑 TCP、Zstd、GUI 和计算开销，建议第一版从：

```
lr-qnative
10 fps
只传 raw + metadata
B 端只显示 depth + ir
```

开始。

如果发送 native raw：

```
raw = 1024 × 4096 × 2 bytes ≈ 8 MB/frame
10 fps ≈ 80 MB/s
30 fps ≈ 240 MB/s
```

这对 1GbE 压力明显更大，除非 Zstd 压缩率很好，否则不建议作为第一版默认模式。

八、工程实现优先级
---------

我建议按这个优先级推进：

```
P0：tof-net-common
  协议头
  TCP send/recv exact
  Zstd compress/decompress
  CRC32
  日志

P1：tof-net-collect
  基于 ADI SDK 初始化相机
  setFrameType
  requestFrame
  getData("raw")
  getData("metadata")
  压缩发送

P2：tof-net-recv-dump
  B 端接收后保存 raw / metadata
  不做 TOFI，不做 GUI

P3：tof-net-tofi-test
  B 端离线 raw-to-depth
  与 A 端本地 depth 对齐

P4：tof-net-viewer
  接入 tof-viewer GUI
  显示 depth / ir / conf

P5：point cloud
  B 端根据 depth + intrinsics 生成 xyz
  不建议 A 端传 xyz
```

九、当前最需要避免的错误
------------

这个项目最容易失败的地方有三个：

```
1. A 端只传 raw，不传 metadata / mode / ini / calibration
2. B 端直接尝试 GUI 显示，但 TOFI 计算还没验证
3. 误以为 raw.bin 可以脱离 RawToDepth 配置独立还原 depth
```

正确路线是：

```
先证明：
A 本地 raw -> A 本地 depth
B 离线 raw -> B 离线 depth
两者一致

再证明：
A 网络 raw -> B 网络 depth
结果稳定

最后：
接入 GUI
```

十、结论
----

你现在这个目标可以明确成一句工程定义：

```
在 ToF rel-4.2.1 中新增网络化 raw-frame 传输层，使机器 A 仅负责相机采集与 raw 数据发送，机器 B 复用 ADI TOFI/RawToDepth 计算链路和 tof-viewer 显示链路，实现 depth / ir / conf / point cloud 的远程实时显示。
```

后续改代码时，优先从 `data-collect` / `first-frame` 的采集逻辑抽取 A 端，从 `tof-viewer` 的 frame 显示逻辑抽取 B 端 GUI，从 SDK 的 RawToDepth / TOFI 路径抽取 B 端计算。ADI 官方仓库也明确包含 `tof-viewer`、`data-collect` 和 `first-frame` 这些示例，适合作为这三个部分的代码来源。[![](https://www.google.com/s2/favicons?domain=https://github.com&sz=128)github.com](https://github.com/analogdevicesinc/ToF)

## Prompt:
现在我需要完整的工程包，给出下载链接，要求完全实现上述内容

## Response:
我理解你的需求：你希望得到一个**完整可编译、运行的双机 A/B 网络 ToF 工程包**，完全实现：

*   A 端采集 raw + metadata 并压缩发送
*   B 端接收 raw + metadata 并调用 TOFI 计算 depth / ir / conf / point cloud
*   GUI 显示整合到 `tof-viewer`
*   协议、压缩、CRC 全部完整实现

* * *

我可以帮你设计**完整工程结构、CMake、示例代码、协议头和压缩/解压模块**，生成文件，然后打包成 zip 下载，但 **我不能直接提供已经存在的 ADI SDK 或 TOFI 算法的二进制**，因为它们受官方授权限制。你需要在本地安装 ToF rel-4.2.1 SDK，然后把我的工程和 SDK 结合编译即可。

我可以生成一个**可直接下载的工程模板包**，里面包括：

```
tof-net-project/
├── tof-net-common/          # 协议、TCP、Zstd、CRC32
├── tof-net-collect/         # 机器 A 采集端示例
├── tof-net-viewer/          # 机器 B 接收 + TOFI + GUI 示例
├── config/                  # ini / JSON 配置示例
├── CMakeLists.txt           # 顶层 CMake
└── README.md                # 编译 & 运行说明
```

每个模块我可以生成 **完整 C++ 代码**，使用 ADI SDK 接口占位函数调用，并带 TCP + Zstd 示例。

* * *

我可以帮你做的下一步：

1.  生成工程模板 zip，文件命名和目录结构与上面一致
2.  每个模块附带示例 `main.cpp`，实现：
    *   A 端采集 raw + metadata + 压缩发送
    *   B 端接收 + 解压 + TOFI 占位计算 + GUI 显示接口
3.  README.md 包含：
    *   依赖安装
    *   编译命令
    *   运行示例

* * *

我需要确认几个细节：

1.  你希望 B 端的 TOFI 计算是**直接调用 SDK**还是**占位模拟**？（如果没有 SDK，我可以生成可编译的占位函数）
2.  GUI 部分，你希望保留原始 `tof-viewer` 界面，还是简化显示 depth/ir/point cloud 三个窗口？
3.  网络传输，你希望**TCP**还是**UDP**？（我推荐 TCP，可靠传输）

确认后，我可以直接生成下载包给你。

## Prompt:
B 端的 TOFI 计算是直接调用 SDK，保留原始 tof-viewer 界面，网络传输采用TCP

## Response:
我会生成可下载的 rel-4.2.1 overlay 工程包，包含 TCP/Zstd/CRC、A 端 raw+metadata 采集发送、B 端 SDK/TOFI 适配和 tof-viewer 集成入口。包内不再分发 ADI 官方源码或二进制，而是通过本地 ToF 树链接构建。

我会把 B 端接成 SDK 级网络相机后端，让原 `tof-viewer` 仍沿 `System→Camera→Frame/requestFrame` 路径工作。A 端则按 SDK 原生初始化、取 `raw` 和 metadata。

底层代码已开始落到 `/mnt/data`，已生成协议、TCP exact 收发、Zstd、CRC、文件 I/O 和 CMake 查找模块。下一步补齐 B 端 TOFI/viewer patch、安装脚本并打包。

已思考 30m 16s

工程包已生成：

下载完整工程包：tof-rel421-net-ab-overlay.zip

这个包是 **ToF rel-4.2.1 源码树 overlay 工程包**，用于覆盖/扩展你本地已有的 ADI ToF rel-4.2.1。它没有重新分发 ADI 官方 SDK、TOFI 二进制库、firmware、CCB、CFG 或标定文件；这些必须继续使用你本地 rel-4.2.1 自带/官方授权的内容。

已实现内容
-----

该工程包实现的是你确认的版本：

```
ToF rel-4.2.1
双机 A/B 架构

机器 A：
  ADI SDK 连接真实 ToF 相机
  requestFrame()
  getData("raw")
  getData("metadata")
  TCP 发送 HELLO / START / FRAME
  Zstd 压缩 raw + metadata
  CRC32 校验

机器 B：
  保留原始 tof-viewer GUI
  新增 RawTcpDepthSensor
  TCP listen / accept
  接收并解压 raw + metadata
  伪装成 ADI DepthSensorInterface
  交给 CameraItof::requestFrame()
  由原 SDK 路径调用 TOFI / RawToDepth
  显示 depth / ir / conf / xyz / point cloud
```

这里 B 端不是占位模拟计算。包里的核心设计是新增 `RawTcpDepthSensor`，实现 ADI 的 `DepthSensorInterface`，然后创建 `CameraItof`。ADI SDK 的 `CameraItof::requestFrame()` 本身会从 `DepthSensorInterface::getFrame()` 取得 raw buffer，并在开启深度计算时调用 `TofiCompute()` 生成 depth / ir / xyz；因此 B 端接收网络 raw 后，会继续走 SDK 原有 TOFI 计算链路。[![](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com&sz=128)GitHub+2![](https://www.google.com/s2/favicons?domain=https://github.com&sz=128)GitHub+2](https://raw.githubusercontent.com/analogdevicesinc/ToF/rel-4.2.1/sdk/include/aditof/depth_sensor_interface.h)

包内目录结构
------

```
tof-rel421-net-ab-overlay/
├── apply_overlay.py
├── README.md
├── LICENSE.txt
├── VERSION
├── MANIFEST.txt
├── tof-net-common/
│   ├── CMakeLists.txt
│   ├── include/tof_net/
│   │   ├── protocol.hpp
│   │   ├── packet_io.hpp
│   │   ├── tcp_socket.hpp
│   │   ├── tof_frame_codec.hpp
│   │   ├── zstd_codec.hpp
│   │   ├── crc32.hpp
│   │   ├── kv.hpp
│   │   ├── hex.hpp
│   │   └── clock.hpp
│   └── src/
│       ├── packet_io.cpp
│       ├── tcp_socket.cpp
│       ├── tof_frame_codec.cpp
│       ├── zstd_codec.cpp
│       ├── crc32.cpp
│       ├── kv.cpp
│       └── hex.cpp
├── examples/
│   ├── tof-net-collect/
│   │   ├── CMakeLists.txt
│   │   └── main.cpp
│   ├── tof-net-recv-dump/
│   │   ├── CMakeLists.txt
│   │   └── main.cpp
│   └── tof-viewer/tof-net/
│       ├── include/
│       │   ├── raw_tcp_depth_sensor.hpp
│       │   ├── tof_net_runtime.hpp
│       │   └── tof_net_viewer_factory.hpp
│       └── src/
│           ├── raw_tcp_depth_sensor.cpp
│           ├── tof_net_runtime.cpp
│           └── tof_net_viewer_factory.cpp
├── docs/
│   ├── PROTOCOL.md
│   └── INTEGRATION.md
├── scripts/
│   ├── build_linux.sh
│   ├── run_a_collect.sh
│   └── run_b_viewer.sh
└── tests/
    └── common_roundtrip.cpp
```

使用方法
----

先解压 overlay 包，然后应用到你的本地 `ToF rel-4.2.1` 源码目录：

```
unzip tof-rel421-net-ab-overlay.zip
cd tof-rel421-net-ab-overlay

python3 apply_overlay.py /path/to/ToF
```

然后进入你的 ToF 源码目录编译：

```
cd /path/to/ToF

sudo apt update
sudo apt install -y libzstd-dev

rm -rf build
cmake -S . -B build
cmake --build build -j$(nproc)
```

`apply_overlay.py` 会自动修改这些位置：

```
/path/to/ToF/CMakeLists.txt
/path/to/ToF/examples/CMakeLists.txt
/path/to/ToF/examples/tof-viewer/CMakeLists.txt
/path/to/ToF/examples/tof-viewer/src/ADIToF.cpp
/path/to/ToF/examples/tof-viewer/src/ADIMainWindow.cpp
```

修改是带 marker 的，脚本按幂等方式 patch，重复执行不会重复插入同一段代码。

B 端运行 tof-viewer
----------------

机器 B 先启动 GUI，并开启 TCP raw camera：

```
cd /path/to/ToF

./build/examples/tof-viewer/ADIToFGUI \
  --tof-net-listen 0.0.0.0 \
  --tof-net-port 5000 \
  --tof-net-frame-type lr-qnative \
  --tof-net-enable-xyz 1
```

GUI 启动后，设备列表里会多出：

```
ToF Net Raw TCP Camera
```

选择这个设备后，继续使用原始 `tof-viewer` 的 Open Device / Start / Display 流程。原 `tof-viewer` 代码本来就是通过 `System::getCameraList()` 得到 camera 列表，再把 camera 接入 GUI 控制逻辑；overlay 只是额外注入一个网络 camera，不重写原 GUI。[![](https://www.google.com/s2/favicons?domain=https://github.com&sz=128)GitHub](https://github.com/analogdevicesinc/ToF/blob/rel-4.2.1/examples/tof-viewer/src/ADIMainWindow.cpp)

A 端运行采集发送
---------

机器 A 连接真实 ToF 相机，然后运行：

```
cd /path/to/ToF

./build/examples/tof-net-collect/tof-net-collect \
  --server <机器B_IP> \
  --port 5000 \
  --config examples/tof-viewer/tof-viewer_config_adsd3500_new_modes.json \
  --frame-type lr-qnative \
  --mode lrqmp \
  --ini-name RawToDepthAdsd3500_lrqmp.ini \
  --fps-limit 10 \
  --zstd-level 1
```

A 端使用 ADI SDK 的标准 `System -> Camera -> Frame` 路径获取数据，包括 `initialize()`、`setFrameType()`、`start()`、`requestFrame()` 和 `Frame::getData()`；这些接口是 ADI SDK 官方暴露的相机采集路径。[![](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com&sz=128)GitHub](https://raw.githubusercontent.com/analogdevicesinc/ToF/rel-4.2.1/sdk/include/aditof/camera.h)

先做链路验证
------

包里还提供了一个不启动 GUI 的 B 端接收落盘工具：

```
./build/examples/tof-net-recv-dump/tof-net-recv-dump \
  --listen 0.0.0.0 \
  --port 5000 \
  --out ./tof_net_dump \
  --frames 30
```

建议第一次联调按这个顺序：

```
1. B 端先跑 tof-net-recv-dump
2. A 端跑 tof-net-collect
3. 确认 raw / metadata / manifest 能正常落盘
4. 再启动 B 端 ADIToFGUI
5. 在 GUI 里打开 ToF Net Raw TCP Camera
6. 验证 depth / ir / xyz 显示
```

当前验证状态
------

我在当前环境中已经完成了这些检查：

```
1. tof-net-common 可独立 CMake 编译
2. TCP/packet/frame/zstd/crc 相关 common roundtrip 测试通过
3. overlay patch 脚本已在 mock ToF 树中测试通过
4. zip 包完整性已检查通过
```

没有在当前环境中完成的部分是：

```
1. 完整 rel-4.2.1 官方 SDK 树编译
2. TOFI 动态库实际调用
3. 真实 ADI ToF 硬件采集
4. tof-viewer GUI 实机显示
```

原因是当前沙盒里没有你的 ADI ToF rel-4.2.1 官方源码树、TOFI 库、相机硬件、CCB/CFG/firmware/标定文件。工程包已经按 SDK 原接口接入；真实编译和运行需要在你的本地 ToF 环境中验证。

重要注意事项
------

A/B 两端必须使用一致的：

```
ToF rel-4.2.1 版本
tof-viewer config JSON
RawToDepth*.ini
CCB / CFG / calibration
frameType，例如 lr-qnative
mode，例如 lrqmp
TOFI 库版本
```

第一版建议先用：

```
frameType: lr-qnative
fps-limit: 10
zstd-level: 1
enableXYZframe: 1
enableDepthCompute: B 端打开
A 端只负责 raw 采集发送
```

这样最容易定位问题：先确认网络 raw 链路稳定，再验证 B 端 SDK/TOFI 计算和原 `tof-viewer` 显示链路。



---
Powered by [ChatGPT Exporter](https://www.chatgptexporter.com)