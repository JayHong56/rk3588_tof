# LoopSplat 数据采集与格式要求

项目链接：

- Project page: https://loopsplat.github.io/
- GitHub: https://github.com/GradientSpaces/LoopSplat

## 一句话结论

LoopSplat 不是吃单张 RGB/Depth 图的模型，而是 RGB-D SLAM + 3D Gaussian Splatting 系统。它需要连续 RGB-D 序列、相机内参、深度尺度、RGB-depth 对齐关系，最好还有每帧相机位姿/轨迹。我们现在的单帧 snapshot 可以做点云可视化和预处理 baseline，但不能直接跑出 LoopSplat 官网那种闭环建图效果。

## 官方代码支持的数据类型

官方 README 写明测试过：

- Replica
- TUM_RGBD
- ScanNet
- ScanNet++

对我们最现实的是整理成 TUM_RGBD 风格，因为格式简单，适合我们自己拍一圈后转数据。

## 推荐我们采集的数据

### 必须采集

1. 连续 RGB 图像序列
   - 格式：PNG 或 JPG
   - 建议分辨率：640x480 起步；如果机器撑得住，可以 1280x720 或当前 1600x1200
   - 要求：曝光尽量固定，不能严重运动模糊

2. 连续 Depth/TOF 深度图序列
   - 格式：16-bit PNG 最方便
   - 深度单位要明确，例如：depth_png_value / depth_scale = 米
   - 无效深度用 0
   - RGB 和 Depth 必须同步，最好每帧时间差 < 30 ms，至少 < 80 ms

3. RGB-depth 对齐
   - 最好直接输出“已经对齐到 RGB 视角的 depth”
   - 即 RGB 像素 `(u, v)` 和 depth 像素 `(u, v)` 对应同一条相机射线
   - 如果 depth 还在 TOF/IR 相机视角，必须保存 RGB 相机和 depth 相机之间的外参

4. 相机内参
   - RGB 相机：`fx, fy, cx, cy`
   - Depth/TOF 相机：`fx, fy, cx, cy`
   - 如果 depth 已经对齐到 RGB 视角，LoopSplat 运行时主要用对齐后 RGB-D 的那一套内参

5. 深度尺度
   - 字段名可叫 `depth_scale`
   - 示例：如果 16-bit PNG 中 1000 表示 1 米，则 `depth_scale=1000`
   - TUM RGB-D 常见是 5000，即 5000 表示 1 米

6. 每帧时间戳
   - RGB 时间戳
   - depth 时间戳
   - 推荐单位：秒 float 或纳秒整数，后处理统一即可

7. 每帧相机位姿/轨迹
   - 最好提供，格式：`timestamp tx ty tz qx qy qz qw`
   - `tx ty tz` 单位：米
   - `qx qy qz qw`：单位四元数
   - 这是 camera-to-world pose，表示 RGB 相机光心在世界坐标系中的姿态
   - 如果暂时没有外部定位，可以先用 SLAM/AprilTag/标定板估计，但质量会影响闭环和建图

### 强烈建议保存

1. 畸变参数
   - `k1, k2, p1, p2, k3`
   - 如果图像已经去畸变，记录为 `[0, 0, 0, 0, 0]`

2. RGB 相机到 depth/TOF 相机外参
   - 4x4 matrix，或者 `R(3x3) + t(3)`
   - 单位：米

3. 原始未压缩数据
   - 原始 RGB
   - 原始 depth
   - 对齐后的 depth-to-RGB
   - 相机 SDK 输出的 confidence/IR 图，如果有，也保存

4. 采集质量日志
   - 帧率
   - 掉帧数
   - RGB-depth 时间差
   - 有效深度比例
   - 相机是否自动曝光/自动白平衡

## 推荐文件结构：TUM_RGBD 风格

建议每次拍摄一个场景，形成一个独立文件夹：

```text
scene_001/
  rgb/
    000000.png
    000001.png
    000002.png
  depth/
    000000.png
    000001.png
    000002.png
  rgb.txt
  depth.txt
  groundtruth.txt
  camera.json
```

`rgb.txt`：

```text
# timestamp rgb_file
1718000000.000000 rgb/000000.png
1718000000.033333 rgb/000001.png
1718000000.066667 rgb/000002.png
```

`depth.txt`：

```text
# timestamp depth_file
1718000000.000500 depth/000000.png
1718000000.033800 depth/000001.png
1718000000.066900 depth/000002.png
```

`groundtruth.txt` 或 `pose.txt`：

```text
# timestamp tx ty tz qx qy qz qw
1718000000.000000 0.000 0.000 0.000 0.000 0.000 0.000 1.000
1718000000.033333 0.010 0.000 0.002 0.000 0.003 0.000 0.999
```

`camera.json`，给我们后处理和写 LoopSplat config 用：

```json
{
  "rgb_width": 640,
  "rgb_height": 480,
  "depth_width": 640,
  "depth_height": 480,
  "rgb_intrinsics": {
    "fx": 525.0,
    "fy": 525.0,
    "cx": 319.5,
    "cy": 239.5
  },
  "depth_intrinsics": {
    "fx": 525.0,
    "fy": 525.0,
    "cx": 319.5,
    "cy": 239.5
  },
  "depth_aligned_to_rgb": true,
  "depth_scale": 1000.0,
  "distortion": [0, 0, 0, 0, 0],
  "T_rgb_depth": [
    [1, 0, 0, 0],
    [0, 1, 0, 0],
    [0, 0, 1, 0],
    [0, 0, 0, 1]
  ]
}
```

## LoopSplat config 中必须填的相机字段

LoopSplat 的 dataset loader 会从 config 里读取这些字段：

```yaml
dataset_name: "tum_rgbd"
cam:
  H: 480
  W: 640
  fx: 525.0
  fy: 525.0
  cx: 319.5
  cy: 239.5
  depth_scale: 1000.0
  distortion: [0, 0, 0, 0, 0]
  crop_edge: 0
```

含义：

- `H, W`：输入给模型的 RGB-D 图像高度和宽度
- `fx, fy, cx, cy`：对齐后的 RGB-D 相机内参
- `depth_scale`：PNG 深度值除以它以后得到米
- `distortion`：畸变参数；如果已去畸变则全 0
- `crop_edge`：是否裁掉图像边缘，默认 0

## 拍摄方式建议

### 目标

LoopSplat 的优势是 loop closure，所以一定要有“走出去再回来”的闭环轨迹。只拍直线、只拍一个角度，效果不会体现出来。

### 建议拍摄动作

1. 选择静态室内场景，先不要有人走动、物体移动。
2. 手持或固定支架移动 RGB-D 相机，从一个起点开始。
3. 绕桌子、房间、物体区域慢慢拍一圈。
4. 最后回到接近起点的位置，形成闭环。
5. 每个地方都要和前后帧有足够重叠，建议相邻帧画面重叠 > 70%。
6. 相机移动要慢，避免快速转头和运动模糊。
7. 场景中放一些有纹理、几何特征明显的物体，不要全白墙、全玻璃、强反光。

### 建议帧数和时长

第一批测试：

- 时长：30-60 秒
- 帧率：10-30 FPS
- 可先抽帧到 5-10 FPS 跑代码
- 有效帧数：200-800 帧比较合适

如果 GPU 内存不够，先降分辨率或抽帧，不要一开始拍几千帧。

## 当前 snapshot 和 LoopSplat 要求的差距

当前已有：

- 单帧 RGB
- 单帧 depth
- 部分 XYZ / XYZRGB 点云
- 部分 RGB-depth 对齐图

还缺：

- 连续多帧序列
- 每帧时间戳文件
- 相机内参 `fx, fy, cx, cy`
- 深度尺度 `depth_scale`
- RGB-depth 外参或明确的 depth_aligned_to_rgb
- 每帧 camera-to-world pose / trajectory
- 闭环拍摄轨迹

所以当前数据可以做“单帧点云着色/渲染 baseline”，但不能直接跑出 LoopSplat 官网级别结果。下一批只要按上面的格式拍一圈，就可以开始转换成 TUM_RGBD 输入并尝试跑 LoopSplat。

## 给采集团队的最小清单

拍摄时请保证每个 scene 至少交付：

- `rgb/`：连续 RGB 图片
- `depth/`：连续 16-bit 深度 PNG
- `rgb.txt`：RGB 时间戳和文件名
- `depth.txt`：depth 时间戳和文件名
- `groundtruth.txt` 或 `pose.txt`：每帧相机位姿
- `camera.json`：内参、外参、depth_scale、畸变参数、分辨率
- 一段说明：相机型号、是否硬件同步、是否 depth 已对齐到 RGB、拍摄帧率、拍摄路线

有了这些，就可以按 LoopSplat 官方命令运行：

```bash
python run_slam.py configs/TUM_RGBD/<our_scene_config>.yaml --input_path <path_to_scene_001> --output_path <output_path>
```
