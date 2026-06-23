# LoopSplat 数据采集 — 需求分析与可行性评估

> 编写日期：2026-06-09
> 基于：ADI ToF SDK rel-4.2.1（二次开发版本），rk3588 ARM64 Debian

---

## 一、需求概述

### 1.1 需求

在现有 ADI ToF 摄像头模组采集数据时，获取相关参数，适配 LoopSplat 的数据格式。

### 1.2 硬件能力边界

**ADI ToF 摄像头 (ADSD3500) 实际输出**：`raw`、`ir`、`depth`、`xyz`

明确硬件限制：

| LoopSplat 需求 | 硬件支持？ | 决定 |
|---|---|---|
| RGB 图像 | ❌ 无 RGB 传感器 | **不做** |
| Depth 图像 | ✅ 16-bit depth | 做 |
| 相机内参 | ✅ SDK 可获取 | 做 |
| depth_scale | ✅ 已知 (mm) | 做 |
| 畸变参数 | ✅ SDK 可获取 | 做 |
| RGB-Depth 对齐 | ❌ 无 RGB 传感器，无从对齐 | **不做** |
| 每帧位姿 (groundtruth) | ❌ 无外部追踪 | **不做** |

### 1.3 实际输出

```text
scene_001/
  depth/            ← 连续 16-bit 深度图 (PNG)
    000000.png
    000001.png
    ...
  depth.txt         ← Depth 时间戳 + 文件名
  camera.json       ← 相机内参/depth_scale/畸变
```

> RGB 相关（`rgb/`、`rgb.txt`、`groundtruth.txt`）均不可用，硬件限制。

---

## 二、现系统能力分析

### 2.1 硬件：ADI ToF 摄像头 (ADSD3500 + Crosby 模组)

| 属性 | 说明 |
|---|---|
| 传感器类型 | 间接 ToF (iToF)，ADSD3500 深度处理器 |
| 连接方式 | USB (RNDIS over USB)，IP: 10.42.0.1 |
| 输出数据类型 | `raw`（原始ADC）、`ir`（红外强度）、`depth`（深度）、`xyz`（点云） |
| **有无 RGB** | ❌ **没有 RGB 传感器** |
| 支持模式 | sr-native, lr-native, sr-qnative, lr-qnative, pcm-native, lr-mixed, sr-mixed |

### 2.2 各模式分辨率

| 模式 | Depth / IR 分辨率 | Raw 分辨率 | 说明 |
|---|---|---|---|
| sr-native | 1024 × 1024 | 1024 × 4096 | 高分辨率短距，推荐用于 LoopSplat |
| sr-qnative | 1024 × 1024 | 1024 × 4096 | 高质量短距 |
| sr-mixed | 1024 × 1024 | 1024 × 4096 | 混合模式短距 |
| lr-native | 512 × 640 | 2560 × 640 | 远距 |
| lr-qnative | 512 × 640 | 2560 × 640 | 高质量远距 |
| lr-mixed | 512 × 640 | 2560 × 512 | 混合模式远距 |
| pcm-native | 512 × 640 (仅IR) | 2560 × 640 | 无深度输出 |

> **推荐使用 sr-native（1024×1024）进行 LoopSplat 采集**，分辨率适中，深度质量好。

### 2.3 现有采集程序

#### data_collect（官方示例）

- 路径：`ToF/examples/data_collect/main.cpp`
- 功能：采集单帧或 N 帧 raw/depth 数据，输出 `.bin` 二进制文件
- 缺陷：**不输出任何元数据**（无内参、无时间戳、无 PNG 格式）

#### tof-net-collect（二次开发）

- 路径：`ToF/examples/tof-net-collect/src/main.cpp`
- 功能：采集 raw 帧，压缩后通过网络发送给 tof-net-viewer
- 优点：**已实现内参导出**（`export_dealias_data`）和 **CCB 校准数据导出**（`export_module_ccb`）
- 优点：**已实现多模式支持**（`switchMode`）
- 缺陷：只发送 raw，不做 depth 计算，不输出文件

#### tof-net-viewer（二次开发）

- 路径：`ToF/examples/tof-net-viewer/`
- 功能：接收 raw 帧，用 TOFI 库计算 depth/IR/XYZ，显示 GUI
- 优点：**已实现完整的 TOFI 初始化流程**（包括 CCD、dealias data 加载）

### 2.4 SDK 可用的关键 API

```cpp
// 获取相机内参 + 畸变 (全在一个结构体里)
Camera::getDetails(CameraDetails &details);

struct IntrinsicParameters {
    float fx, fy;     // 焦距
    float cx, cy;     // 主点
    float codx, cody; // 畸变中心
    float k1, k2, k3, k4, k5, k6; // 径向畸变
    float p1, p2;     // 切向畸变
};

struct CameraDetails {
    IntrinsicParameters intrinsics; // 内参 ← 核心
    int maxDepth;  // 最大深度 (毫米)
    int minDepth;  // 最小深度 (毫米)
    int bitCount;  // 像素位数
    FrameDetails frameType; // 帧类型（含分辨率、数据类型列表）
    // ...
};

// 获取帧数据
Frame::getData("depth", &pData); // 16-bit 深度图 (毫米)
Frame::getData("ir", &pData);    // 16-bit 红外强度图
Frame::getData("xyz", &pData);   // 16-bit × 3 点云
Frame::getData("raw", &pData);   // 原始 ADC 数据

// 获取每模式内参（从 ADSD3500 硬件直接读取）
DepthSensorInterface::adsd3500_read_payload_cmd(0x01, intrinsics, 56);
DepthSensorInterface::adsd3500_read_payload_cmd(0x02, dealiasParams, 32);
// → TofiXYZDealiasData 结构体，含 CameraIntrinsics

// 导出模块 CCB（校准数据块，含几何标定）
Camera::setControl("saveModuleCCB", filepath);
```

### 2.5 CCB 中的几何标定数据

CCB 文件（二进制格式）中包含 `CAL_GEOMETRIC_BLOCK_V3`：

```c
struct CAL_GEOMETRIC_BLOCK_V3 {
    uint32_t CameraModel;
    float Fc1, Fc2;    // fx, fy
    float cc1, cc2;    // cx, cy
    float Cx, Cy;      // 畸变中心
    float Kc1~Kc6;     // 径向畸变
    float Tx, Ty;      // 切向畸变
};
```

---

## 三、实现范围

### 3.1 要做的事

| 项目 | 实现方式 |
|---|---|
| Depth 16-bit PNG 序列 | `frame.getData("depth")` → 写 PNG，单位毫米 |
| camera.json | `Camera::getDetails().intrinsics` → fx/fy/cx/cy/k1~k6/p1/p2，depth_scale=1000 |
| depth.txt | 每帧记录 `timestamp_ns depth/000000.png` |
| 连续序列采集 | 循环 `requestFrame()`，`%06d.png` 命名 |
| 采集质量日志 | 帧率、掉帧、有效深度比例（depth>0 的像素占比） |
| 多模式切换 | 复用现有 `switchMode` |

### 3.2 不做的事

| 项目 | 原因 |
|---|---|
| rgb/ 目录 + RGB 图像 | 硬件无 RGB 传感器 |
| rgb.txt | 无 RGB 数据 |
| groundtruth.txt（相机位姿） | 无外部追踪系统 |
| RGB-Depth 外参矩阵 | 无 RGB 传感器，对齐关系无意义 |
| IR 图像作为 RGB 替代 | IR ≠ RGB，硬塞进去会误导 LoopSplat |

---

## 四、实现方案

### 4.1 推荐方案：新建 tof-loopsplat-collect

基于 `tof-net-collect` 的相机初始化 + 内参导出代码，新建本地采集程序。**不修改现有代码**。

### 4.2 程序接口

```
tof-loopsplat-collect
  输入:
    --ip 10.42.0.1
    --m 0                   (模式: 0=sr-native 推荐)
    --n 300                 (采集帧数)
    --fps 10                (目标帧率)
    --output ./scene_001    (输出目录)
    config_crosby_adsd3500_new_modes.json

  输出:
    scene_001/
      depth/000000.png ...  (16-bit 深度图 PNG)
      depth.txt             (时间戳 + 文件名)
      camera.json            (内参/depth_scale/畸变)
      collect_log.txt        (采集质量日志)
```

### 4.3 数据流

```
相机 (ADSD3500)
    │
    ├─ requestFrame()
    │     └─ getData("depth") ──→ 16-bit depth (mm)
    │
    ├─ getDetails().intrinsics ──→ fx, fy, cx, cy, k1~k6, p1, p2
    │
    └─ 输出
          ├─ depth/*.png       (stb_image_write)
          ├─ depth.txt         (逐帧追加)
          └─ camera.json       (一次性写入)
```

### 4.4 采集流程

```
1. 相机初始化 (同 data_collect 流程)
2. camera->setControl("enableDepthCompute", "on")   ← 需要 depth 输出
3. camera->setFrameType(modeName)
4. camera->start()
5. getDetails() → 写 camera.json
6. 循环 requestFrame():
   a. frame.getData("depth", &pData)
   b. 获取时间戳 (std::chrono::steady_clock)
   c. stbi_write_png("depth/%06d.png", pData, 16-bit)
   d. 追加 depth.txt: "timestamp depth/%06d.png"
   e. 统计帧率、有效深度比例
7. camera->stop()
8. 写 collect_log.txt
```

### 4.5 camera.json

```json
{
  "camera_model": "ADI_ADSD3500_Crosby",
  "depth_width": 1024,
  "depth_height": 1024,
  "depth_intrinsics": {
    "fx": 525.0,
    "fy": 525.0,
    "cx": 512.0,
    "cy": 512.0
  },
  "depth_scale": 1000.0,
  "depth_unit": "millimeter",
  "distortion": [-0.1, 0.2, -0.001, 0.001, -0.05]
}
```

> 内参实际值从硬件读取，以上为示意。

### 4.6 深度值的处理

- SDK 输出 depth 为 16-bit 无符号整数，单位**毫米**
- 写入 16-bit PNG 时保持原始值
- `depth_scale = 1000`（PNG值/1000 = 米）
- 值为 0 的像素 = 无效深度

---

## 五、实现成本估算

### 5.1 代码量

| 模块 | 行数 | 说明 |
|---|---|---|
| 命令行参数解析 | ~60 | 复用 tof-net-collect 风格 |
| 相机初始化 | ~60 | 复用 CameraRawSource |
| 帧采集循环 | ~60 | requestFrame + getData("depth") |
| 16-bit PNG 写入 | ~30 | stb_image_write |
| camera.json 生成 | ~60 | 手工 JSON 序列化 |
| depth.txt 写入 | ~20 | 逐帧追加 |
| 采集日志 | ~30 | 帧率/有效深度比例 |
| CMakeLists.txt | ~25 | 新增 target |
| **合计** | **~345 行** | |

### 5.2 依赖

| 依赖 | 状态 |
|---|---|
| aditof SDK | ✅ 已安装 |
| stb_image_write.h | ✅ 项目中已有 |
| glog | ✅ 已安装 |

**无需新增任何依赖。**

### 5.3 工作量

| 阶段 | 时间 |
|---|---|
| 编码 | 1 天 |
| rk3588 编译测试 | 0.5 天 |
| 采集验证 | 0.5 天 |
| **总计** | **~2 天** |

---

## 六、风险

| 风险 | 缓解 |
|---|---|
| depth 帧率不稳定 | 降低目标帧率（5-10 FPS），用实际时间戳而非假定帧间隔 |
| rk3588 存储写入成为瓶颈 | 异步写入磁盘，或先写 raw 后用后处理脚本转 PNG |
| 无 RGB 导致 LoopSplat 无法直接使用 | 数据可做点云重建，RGB 需后续硬件升级 |

## 七、下一步

1. 新建 `tof-loopsplat-collect`（~345 行代码）
2. 实现 depth PNG 序列输出 + camera.json + depth.txt
3. 上 rk3588 编译测试
4. 采集一小段场景验证格式和数据质量
5. 确认输出格式是否满足需求

---

## 附录 A：关键源文件索引

| 文件 | 路径 | 说明 |
|---|---|---|
| data_collect 主程序 | `ToF/examples/data_collect/main.cpp` | 官方采集示例 |
| tof-net-collect 主程序 | `ToF/examples/tof-net-collect/src/main.cpp` | 网络采集端（**有内参导出**） |
| tof-net-viewer TOFI 流处理 | `ToF/examples/tof-net-viewer/src/ADINetworkToFStream.cpp` | 网络显示端（**有完整的深度计算流程**） |
| tof-net-common 协议 | `ToF/examples/tof-net-common/` | 网络协议定义 |
| Camera 接口 | `ToF/sdk/include/aditof/camera.h` | 相机 API |
| Camera 定义 | `ToF/sdk/include/aditof/camera_definitions.h` | **IntrinsicParameters, CameraDetails** |
| Frame 接口 | `ToF/sdk/include/aditof/frame.h` | 帧 API |
| Frame 定义 | `ToF/sdk/include/aditof/frame_definitions.h` | **FrameDetails, FrameDataDetails** |
| 内参结构体 | `ToF/sdk/common/adi/tofi/tofi_camera_intrinsics.h` | **CameraIntrinsics, TofiXYZDealiasData** |
| 标定类型 | `ToF/sdk/common/adi/ccb/include/TOF_Calibration_Types.h` | **CAL_GEOMETRIC_BLOCK_V3** (CCB) |
| CameraItof 实现 | `ToF/sdk/src/cameras/itof-camera/camera_itof.cpp` | getDetails 实现、内参读取 |
| 模式信息 | `ToF/sdk/src/cameras/itof-camera/mode_info.h` | 各模式分辨率/参数表 |
| ADSD3500 传感器 | `ToF/sdk/src/connections/target/adsd3500_sensor.h` | 传感器帧类型定义 |

## 附录 B：LoopSplat 引用

- Project: <https://loopsplat.github.io/>
- GitHub: <https://github.com/GradientSpaces/LoopSplat>
- 输入格式: TUM_RGBD, Replica, ScanNet, ScanNet++
- 配置文件: `configs/TUM_RGBD/<scene>.yaml`
- 运行命令: `python run_slam.py configs/TUM_RGBD/<config>.yaml --input_path <path> --output_path <output>`
