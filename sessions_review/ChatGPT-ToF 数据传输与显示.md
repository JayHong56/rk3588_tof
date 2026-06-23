# ToF 数据传输与显示

**User:** Anonymous  
**Created:**   
**Updated:**   
**Exported:** 6/13/2026 14:13:46  
**Link:** [https://chatgpt.com/share/6a0da277-6cf8-83ab-9b38-85e181841257](https://chatgpt.com/share/6a0da277-6cf8-83ab-9b38-85e181841257)  

## Prompt:
上传文件

## Prompt:
上传文件

修改我上传的文件，使其符合我的需求

## Response:
我会重新处理你上传/上次生成的工程包：保留 `tof-viewer` 的目录和 GUI 基础，但移除我之前自定义的 `TofiDepthEngine` 封装，不再提供 `tofi_depth_engine.hpp/.cpp`。深度计算调用会改为直接使用 ToF/TOFI 提供的接口。

我会在你上传包的基础上做具体改动：删除 `include/tofi_depth_engine.hpp` 和 `src/tofi_depth_engine.cpp`，`CMakeLists.txt` 不再编译它们；`main.cpp` 直接包含 `tofi_config.h/tofi_compute.h` 并直接调用 `InitTofiConfig()`、`InitTofiCompute()`、`TofiCompute()`。

已思考 2m 11s

已重新修改 `tof_net_viewer`，去掉了自定义的 `TofiDepthEngine`。

下载：

*   完整修改包
*   仅 tof-net-viewer 目录
*   仅 tof-net-common 目录

修改点：

```
删除：
  examples/tof-net-viewer/include/tofi_depth_engine.hpp
  examples/tof-net-viewer/src/tofi_depth_engine.cpp

修改：
  examples/tof-net-viewer/CMakeLists.txt
    不再编译 src/tofi_depth_engine.cpp

  examples/tof-net-viewer/src/main.cpp
    直接包含：
      tofi_config.h
      tofi_compute.h
      tofi_error.h

    直接调用：
      InitTofiConfig()
      InitTofiCompute()
      TofiCompute()
      FreeTofiCompute()
      FreeTofiConfig()
```

新的 `tof_net_viewer` 仍保持边界：

```
基于 tof-viewer 的 GUI/配置/显示方式
不枚举本地 ToF
不初始化本地 ToF
不启动本地 ToF
不调用 Camera::requestFrame()
不链接 aditof
只监听网络、解压 raw frame、调用 ADI TOFI API 计算并显示
```

构建方式不变：

```
rm -rf build
cmake -S . -B build
cmake --build build -j$(nproc)
```

运行方式不变：

```
./build/examples/tof-net-viewer/tof_net_viewer \
  ./build/examples/tof-viewer_config_adsd3500_new_modes.json \
  --listen-ip 192.168.3.32 \
  --expected-a-ip 192.168.3.146 \
  --port 5000 \
  --frame-type lr-qnative \
  --mode 0
```

## Prompt:
./build/examples/tof-net-viewer/tof_net_viewer \
>   ./build/examples/tof-viewer_config_adsd3500_new_modes.json \
>   --listen-ip 192.168.3.32 \
>   --expected-a-ip 192.168.3.146 \
>   --port 5000 \
>   --frame-type lr-qnative \
>   --mode 0
warning: unable to resolve path: ./config/camera_calibration_adsd3500.ccb
searched:
  ./build/examples/./config/camera_calibration_adsd3500.ccb
  ./build/examples/tof-net-viewer/./config/camera_calibration_adsd3500.ccb
  ./build/examples/tof-net-viewer/.././config/camera_calibration_adsd3500.ccb
  ./build/examples/tof-net-viewer/../tof-net-viewer/./config/camera_calibration_adsd3500.ccb
  ./build/examples/tof-net-viewer/../tof-viewer/./config/camera_calibration_adsd3500.ccb
  ./build/examples/tof-net-viewer/../config/./config/camera_calibration_adsd3500.ccb
  ./config/camera_calibration_adsd3500.ccb
fatal: calibration CCB not found: ./build/examples/./config/camera_calibration_adsd3500.ccb
tof_net_viewer [tof-viewer_config.json] [options]

The JSON input keeps the same style as examples/tof-viewer/tof-viewer_config*.json.
It should contain fields such as CCB_Calibration and DEPTH_INI.
Split-network parameters are command-line only.

Options:
  --config <json>        Viewer config JSON. Default: tof-net-viewer_config.json.
  --listen-ip <B_IP>     Local IP on Machine B to listen on.
  --expected-a-ip <IP>   Accept only Machine A from this IP. Empty = accept any.
  --port <PORT>          TCP port.
  --mode <n>             TOFI mode used by InitTofiConfig and sent to A.
  --frame-type <name>    Frame type sent to A when Start is pressed.
  --min-depth <mm>       Display min depth.
  --max-depth <mm>       Display max depth.
  --cal-file <ccb>       Optional override for CCB_Calibration.
  --ini-file <ini>       Optional override for selected DEPTH_INI.
  -h, --help             Show this help.

展开收起

## Response:
这个错误说明当前程序已经按规则推断出 `camera_calibration_adsd3500.ccb`，但你的机器 B 构建目录里没有这个文件。这里需要两处处理：一是路径搜索逻辑要兼容原 `tof-viewer` 的 `./config/...` 写法；二是如果实际没有 CCB，就必须从机器 A 的模组导出或显式指定。

我会做两项修改：第一，`tof_net_viewer` 不再因为找不到 CCB 直接退出，而是在 GUI 中显示缺失状态；第二，增加兼容原 `tof-viewer` 的路径查找，并支持后续从 `tof_net_collect` 发送 CCB。这样 viewer 本身仍不操作 ToF。

已思考 2m 11s

已重新修改 `tof_net_viewer`，处理了你现在遇到的 CCB 路径问题，并保持你的边界要求：

*   基于 `tof-viewer` 的 GUI/配置/显示方式。
*   不操作本地 ToF。
*   不使用自定义 `TofiDepthEngine`。
*   直接调用 ADI TOFI C API：`InitTofiConfig()`、`InitTofiCompute()`、`TofiCompute()`。
*   如果 CCB 文件暂时缺失，GUI 仍会启动，并在 Network/Status 窗口显示 `TOFI initialized: no` 和具体错误；不会一启动就退出。

下载修正版：

下载 tof\_net\_viewer\_no\_custom\_tofidepthengine\_path\_lazyfix.zip

这版还修正了路径搜索逻辑，会同时尝试：

```
build/examples/config/camera_calibration_adsd3500.ccb
build/examples/tof-net-viewer/config/camera_calibration_adsd3500.ccb
build/examples/tof-viewer/config/camera_calibration_adsd3500.ccb
```

你的命令可以继续使用：

```
./build/examples/tof-net-viewer/tof_net_viewer \
  ./build/examples/tof-viewer_config_adsd3500_new_modes.json \
  --listen-ip 192.168.3.32 \
  --expected-a-ip 192.168.3.146 \
  --port 5000 \
  --frame-type lr-qnative \
  --mode 0
```

但要真正完成 raw → depth 计算，机器 B 仍然必须有 CCB。先查：

```
find ./build ./sdk ./examples -name "camera_calibration_adsd3500.ccb"
```

如果没有，需要从机器 A 导出模组 CCB，然后拷到机器 B，例如：

```
./build/examples/tof-net-collect/tof_net_collect \
  --ip 10.42.0.1 \
  --m 0 \
  --ccb ./camera_calibration_adsd3500.ccb \
  config/config_crosby_adsd3500_new_modes.json \
  --server-ip 192.168.3.32 \
  --bind-ip 192.168.3.146 \
  --port 5000
```

然后拷到机器 B：

```
scp camera_calibration_adsd3500.ccb user@192.168.3.32:/home/omnisky/fpga/wzz/workspace/tof_build/ToF/build/examples/config/
```

或者启动时显式指定：

```
--cal-file /path/to/camera_calibration_adsd3500.ccb
```

## Prompt:
借助tof-viewer如何获取ccb文件

## Response:
我会查一下 `tof-viewer` 里是否已有保存 CCB 的 UI/控制调用；如果有，就给你 GUI 操作路径和等价的命令行方式。

正在调用工具

已思考 19s

可以，但**不是通过 `tof-viewer` GUI 直接导出**。在 rel-4.2.1 代码里，导出 CCB 的标准方式是 `data_collect` 的 `--ccb` 参数：它会在初始化相机后调用：

```
camera->setControl("saveModuleCCB", ccbFilePath);
```

也就是说，CCB 是从机器 A 上连接的 ToF 模组中导出的。`data_collect` 的源码里明确解析了 `--ccb` 参数，并把路径保存到 `ccbFilePath`；初始化相机后，如果该路径非空，就调用 `saveModuleCCB` 保存 CCB。

推荐做法：在机器 A 上导出 CCB
------------------

你的 ToF 接在机器 A，机器 A IP 是 `192.168.3.146`，ToF IP 是 `10.42.0.1`。在机器 A 的 ToF 根目录执行：

```
cd /home/omnisky/fpga/wzz/workspace/tof_build/ToF

./build/examples/data_collect/data_collect \
  --ip 10.42.0.1 \
  --m 0 \
  --ccb ./camera_calibration_adsd3500.ccb \
  config/config_crosby_adsd3500_new_modes.json
```

如果你的 `data_collect` 可执行文件实际在 `build/examples/data_collect/` 下，上面命令就是正确路径。若你是在 `build/examples/` 目录下执行，则改成：

```
./data_collect \
  --ip 10.42.0.1 \
  --m 0 \
  --ccb ./camera_calibration_adsd3500.ccb \
  config/config_crosby_adsd3500_new_modes.json
```

执行成功后，机器 A 当前目录会生成：

```
camera_calibration_adsd3500.ccb
```

拷贝到机器 B
-------

在机器 A 上执行：

```
scp ./camera_calibration_adsd3500.ccb user@192.168.3.32:/home/omnisky/fpga/wzz/workspace/tof_build/ToF/build/examples/config/
```

把 `user` 换成机器 B 的实际用户名。如果目录不存在，先在机器 B 上创建：

```
mkdir -p /home/omnisky/fpga/wzz/workspace/tof_build/ToF/build/examples/config
```

机器 B 启动 tof\_net\_viewer
------------------------

之后机器 B 就可以这样启动：

```
./build/examples/tof-net-viewer/tof_net_viewer \
  ./build/examples/tof-viewer_config_adsd3500_new_modes.json \
  --listen-ip 192.168.3.32 \
  --expected-a-ip 192.168.3.146 \
  --port 5000 \
  --frame-type lr-qnative \
  --mode 0 \
  --cal-file ./build/examples/config/camera_calibration_adsd3500.ccb
```

如果你坚持“借助 tof-viewer”
--------------------

`tof-viewer` 本身主要用于 GUI 显示和相机操作；我目前没有在 rel-4.2.1 的 `tof-viewer` 入口代码中看到直接的 “Export CCB” 命令行路径。实际可复用的是同一个 SDK 控制项：

```
saveModuleCCB
```

所以最稳妥的是用官方 `data_collect --ccb` 导出 CCB，再给 `tof_net_viewer` 使用。



---
Powered by [ChatGPT Exporter](https://www.chatgptexporter.com)