# tof-loopsplat-collect 实现原理

## 一、定位

在现有代码体系中的位置：

```
data_collect         官方的，输出 .bin，无元数据
tof-net-collect     二次开发的，发 raw 给 viewer，enableDepthCompute=off
tof-net-viewer      二次开发的，收 raw → TOFI 计算 → 显示
tof-loopsplat-collect  ← 本程序，本地 depth PNG + 元数据
```

与 tof-net-collect 最接近，复用了其相机初始化流程，核心差异是 `enableDepthCompute=on`。

---

## 二、整体流程

```
┌─────────────┐    ┌─────────────┐    ┌──────────────┐
│ ADSD3500    │───▶│ SDK 内部    │───▶│ 本程序       │
│ USB 摄像头   │    │ TofiCompute │    │              │
└─────────────┘    └─────────────┘    └──────┬───────┘
                                             │
                   ┌─────────────────────────┼──────────────────┐
                   │                         ▼                  │
                   │   camera.json    depth/000000.png          │
                   │   depth.txt      collect_log.txt           │
                   │   (一次性写入)    (逐帧写入)                 │
                   └────────────────────────────────────────────┘
```

---

## 三、相机初始化

### 3.1 完整链路

```
1. System::getCameraListAtIp(cameras, ip)
   └── 通过 RNDIS 网卡连接 USB 摄像头 (固定 IP 10.42.0.1)

2. camera->setControl("initialization_config", json_path)
   └── 设置初始化参数，json 中指定了 DEPTH_INI、FPS 等

3. camera->initialize()
   ├── 枚举 ADSD3500 传感器（RNDIS / V4L2）
   ├── 读取固件版本
   ├── adsd3500_read_payload_cmd(0x01) → CameraIntrinsics
   │   └── fx, fy, cx, cy, k1-k6, p1, p2, codx, cody
   ├── adsd3500_read_payload_cmd(0x02) → Dealias 参数
   │   └── 频率、binning、有效区域、传感器尺寸
   ├── 读取 EEPROM 中的 CCB 校准数据
   └── 设置 imagerType, modeVersion

4. camera->getDetails(camDetails)
   └── 返回 CameraDetails，其中：
       ├── intrinsics     ← 从步骤3的 cmd 0x01 填充
       ├── frameType      ← width, height, dataDetails[]
       ├── maxDepth/minDepth (mm)
       └── cameraId, mode, connection
```

### 3.2 内参从哪来

两路都能拿，结果一致：

| 路径 | API | 时机 |
|---|---|---|
| getDetails | `camera->getDetails().intrinsics` | initialize 之后任意时刻 |
| 硬件直读 | `adsd3500_read_payload_cmd(0x01)` | initialize 内部自动调用 |

本程序走 `getDetails()`，更简洁。结构体：

```cpp
struct IntrinsicParameters {
    float fx, fy;     // 焦距 (pixel)
    float cx, cy;     // 主点 (pixel)
    float codx, cody; // 畸变中心
    float k1~k6;      // 径向畸变
    float p1, p2;     // 切向畸变
};
```

LoopSplat 用到的前 5 个畸变系数：`k1, k2, p1, p2, k3`。

---

## 四、深度数据获取

### 4.1 enableDepthCompute

```
enableDepthCompute=off  → frame.getData("depth") 返回 nullptr
                         （tof-net-collect 用这个，只拿 raw）

enableDepthCompute=on   → SDK 初始化 TOFI 计算库
                         → requestFrame 内部自动调 TofiCompute
                         → depth/ir/xyz 数据可用
```

### 4.2 帧内数据流

```
camera->requestFrame(&frame)
    │
    ├── m_depthSensor->getFrame(raw_buffer)     // V4L2 取 raw 数据
    │
    ├── [enableDepthCompute=on]
    │   └── TofiCompute(raw_buffer, tofi_ctx)  // 深度计算
    │       ├── tofi_ctx->p_depth_frame  ← 16-bit 深度 (毫米)
    │       ├── tofi_ctx->p_ab_frame     ← 16-bit 主动亮度/IR
    │       └── tofi_ctx->p_xyz_frame    ← 16-bit × 3 点云
    │
    └── frame.getData("depth", &ptr)  → 指向 p_depth_frame
```

### 4.3 深度值含义

```
depth[u,v] = 0      → 无效（无信号 / 超出范围 / 被遮挡）
depth[u,v] = 1000   → 1 米
depth[u,v] = 5000   → 5 米
depth_scale = 1000  （PNG 像素值 / 1000 = 米）
```

---

## 五、PNG 写入

### 5.1 为什么用 OpenCV

```
cv::Mat depth_mat(height, width, CV_16UC1, depthData);
cv::imwrite("depth/000000.png", depth_mat);
```

两行搞定 16-bit 无损 PNG。OpenCV 内部链路：

```
cv::imwrite
  └── cv::imencode(".png", mat)
      └── libpng (系统自带)
          ├── IHDR: 16-bit grayscale
          ├── IDAT: zlib 压缩（无损）
          └── IEND
```

### 5.2 为什么不是 raw 而是 PNG

| 方案 | 优点 | 缺点 |
|---|---|---|
| 直接写 `.bin` | 零开销 | 需后处理脚本，别人看不懂 |
| 16-bit PNG | 通用格式、无损、自描述 | CPU 稍高 |

PNG 压缩对 depth 图通常 1.5:1 ~ 3:1，sr-native 每帧约 1-2MB。

---

## 六、输出文件设计

### 6.1 depth.txt

每帧一行，两个字段：

```
<timestamp_ns> <relative_path>
```

时间戳用 `std::chrono::steady_clock`（单调时钟），纳秒精度。没用 `system_clock`，因为 steady_clock 不受系统时间调整影响。

### 6.2 camera.json

只在采集开始时写一次。内参来自 `getDetails().intrinsics`，畸变系数按 LoopSplat 约定只取 `[k1, k2, p1, p2, k3]`。

### 6.3 collect_log.txt

采集结束后的汇总：

```
frames_captured: 298
frames_dropped: 2
total_duration_s: 30
average_fps: 9.93
valid_depth_pct: 87.3
```

每 30 帧的实时进度同时打到 stdout/glog。

---

## 七、采集循环

```
while (未达到 n_frames 且 未收到 SIGINT) {

    1. camera->requestFrame(&frame)
       └── 阻塞等待下一帧（帧率由相机 FPS 设置决定，默认 10 FPS）

    2. frame.getData("depth", &depthData)
       └── depthData 指向 1024×1024 个 uint16_t

    3. 写 PNG
       └── cv::imwrite

    4. 追加 depth.txt
       └── timestamp + 相对路径

    5. 统计
       └── 有效像素数 (depth > 0) / 总像素数
}
```

**没有多线程**——PNG 写入和 txt 追加在主线程同步完成。10 FPS 下每帧间隔 100ms，PNG 写入约 10-20ms，不会成为瓶颈。如果未来帧率提高，可以改造为异步写入。

---

## 八、与另外两个采集程序的关键差异

| | data_collect | tof-net-collect | tof-loopsplat-collect |
|---|---|---|---|
| 深度计算 | 可选 | **off**（只抓 raw） | **on** |
| 输出格式 | .bin 二进制 | Zstd 压缩 raw → TCP | 16-bit PNG 文件 |
| 内参输出 | 无 | 有（发到 viewer） | 有（写 camera.json） |
| 时间戳 | 无 | 有（纳秒，在 TCP header） | 有（纳秒，在 depth.txt） |
| 网络 | 无 | TCP 客户端 | 无 |
| 用途 | 原始数据 dump | 远程实时显示 | 离线场景重建 |

---

## 九、错误处理

| 异常 | 行为 |
|---|---|
| 未检测到相机 | `LOG(ERROR)` + 退出码 1 |
| 初始化失败 | `LOG(ERROR)` + 退出码 1 |
| 选了 pcm-native | 退出码 1（无深度输出） |
| 单帧 requestFrame 失败 | `LOG(ERROR)` + drop_count++ + 继续 |
| SIGINT / SIGTERM | 优雅停止，正常写 log |
| 磁盘满 | cv::imwrite 抛异常，终止采集 |
