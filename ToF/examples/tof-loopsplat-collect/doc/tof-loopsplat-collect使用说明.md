# tof-loopsplat-collect 编译与使用说明

## 1. 编译

在 rk3588 上，进入 ToF的build 目录，配置并编译：

```bash
cd ~/rk3588_tof/ToF/build

# 配置（只需一次）
cmake .. \
    -DWITH_EXAMPLES=ON \
    -DWITH_NETWORK=ON \
    -DUSE_DEPTH_COMPUTE_OPENSOURCE=ON \
    -DUSE_DEPTH_COMPUTE_STUBS=OFF \
    -DCMAKE_PREFIX_PATH="/opt/glog;/opt/protobuf;/opt/websockets"

# 编译（之后改代码只需要这一步）
make tof_loopsplat_collect -j$(nproc)
```

编译产物：`build/examples/tof-loopsplat-collect/tof_loopsplat_collect`

## 2. 使用

### 2.1 基本命令

```bash
cd ~/rk3588_tof/ToF/build

./examples/tof-loopsplat-collect/tof_loopsplat_collect \
    config/config_crosby_adsd3500_new_modes.json \
    --ip 10.42.0.1 \
    --m 0 \
    --n 300 \
    --output ./scene_001
```

### 2.2 参数说明

| 参数 | 说明 | 默认值 |
|---|---|---|
| `CONFIG_JSON`（位置参数） | 相机初始化 JSON 配置文件 | 必填 |
| `--ip` | 相机 IP 地址 | USB 枚举（本地设备） |
| `--m` | 模式编号 | 0 |
| `--n` | 采集帧数 | 0（不限，Ctrl+C 停止） |
| `--output` | 输出目录 | `./scene_001` |
| `--wt` | 预热秒数 | 0 |
| `--fw` | ADSD3500 固件文件（仅升级用） | 无 |
| `--ext_fsync` | 外部同步 | 0（内部时钟） |

### 2.3 模式对照

| --m | 名称 | Depth 分辨率 | 说明 |
|---|---|---|---|
| 0 | sr-native | 1024×1024 | **推荐**，短距高分辨率 |
| 1 | lr-native | 1024×1024 | 远距 |
| 2 | sr-qnative | 512×512 | 短距高质量 |
| 3 | lr-qnative | 512×512 | 远距高质量 |
| 4 | pcm-native | 无 depth | ❌ 不能用，无深度输出 |

### 2.4 使用示例

```bash
# 采集 10 帧快速验证
./examples/tof-loopsplat-collect/tof_loopsplat_collect \
    config/config_crosby_adsd3500_new_modes.json \
    --ip 10.42.0.1 --m 0 --n 10 --output ./test

# 采集 300 帧 (~30s @10FPS)，存到数据盘
./examples/tof-loopsplat-collect/tof_loopsplat_collect \
    config/config_crosby_adsd3500_new_modes.json \
    --ip 10.42.0.1 --m 0 --n 300 --output /home/linaro/data/desk_scene

# 无限采集，手动 Ctrl+C 停止
./examples/tof-loopsplat-collect/tof_loopsplat_collect \
    config/config_crosby_adsd3500_new_modes.json \
    --ip 10.42.0.1 --m 0 --output /home/linaro/data/room_loop
```

## 3. 输出格式

```text
scene_001/
├── depth/
│   ├── 000000.png      16-bit 灰度 PNG，单位毫米
│   ├── 000001.png      值 / 1000 = 米
│   ├── 000002.png      值为 0 = 无效深度
│   └── ...
├── depth.txt           时间戳 + 文件名
├── camera.json          内参 / 畸变 / depth_scale
└── collect_log.txt      采集质量日志
```

### 3.1 camera.json

```json
{
  "camera_model": "ADI_ADSD3500_Crosby",
  "mode": "sr-native",
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

> 内参和畸变值从 ADSD3500 硬件实时读取，以上数值仅为示意。

### 3.2 depth.txt

```text
# timestamp_ns depth_file
# depth_scale = 1000 (value / 1000 = meters)
# depth_unit = millimeter
1718000000000000000 depth/000000.png
1718000000033333333 depth/000001.png
1718000000066666666 depth/000002.png
```

### 3.3 用 Python 读取深度图验证

```python
import cv2
import numpy as np

depth = cv2.imread("depth/000000.png", cv2.IMREAD_UNCHANGED)
# depth.dtype == np.uint16
# depth[i,j] / 1000.0 == 米
# depth[i,j] == 0 → 无效
```

## 4. 注意事项

1. **需要 sudo** — 与 data_collect 一样，访问 USB 摄像头需要 root
2. **pcm-native 不可用** — 该模式无深度输出
3. **首次运行需预热** — 建议 `--wt 5` 让相机稳定后再采
4. **大分辨率注意存储** — sr-native 每帧 depth PNG ≈ 2MB，300 帧 ≈ 600MB
