# ToF SDK在ARM上的安装运行流程

## SDK安装

首先在home下创建工作区根目录

```
mkdir rk3588_tof
cd rk3588_tof
```

之后先安装依赖

可以参考https://github.com/analogdevicesinc/ToF/blob/rel-4.2.1/doc/itof/linux_build_instructions.md，但是他这个是使用闭源计算库的教程，我们这里使用开源计算库，其他都一致

https://github.com/analogdevicesinc/libaditof.git即开源计算库

```
sudo apt install cmake

sudo apt install libgl1-mesa-dev libglfw3-dev

sudo apt install libopencv-contrib-dev
sudo apt install libopencv-dev

git clone --branch v0.6.0 --depth 1 https://github.com/google/glog
cd glog
mkdir build_0_6_0 
cd build_0_6_0
cmake -DWITH_GFLAGS=off -DCMAKE_INSTALL_PREFIX=/opt/glog ..
sudo cmake --build . --target install

cd ../../

git clone --branch v3.1-stable --depth 1 https://github.com/warmcat/libwebsockets
cd libwebsockets
mkdir build_3_1 && cd build_3_1
cmake -DLWS_WITH_SSL=OFF -DLWS_STATIC_PIC=ON -DCMAKE_INSTALL_PREFIX=/opt/websockets ..
sudo cmake --build . --target install

cd ../../

git clone --branch v3.9.0 --depth 1 https://github.com/protocolbuffers/protobuf
cd protobuf
mkdir build_3_9_0 && cd build_3_9_0
cmake -Dprotobuf_BUILD_TESTS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=/opt/protobuf ../cmake
sudo cmake --build . --target install

cd ../../

git clone -b rel-4.2.1 https://github.com/analogdevicesinc/libaditof.git

cd libaditof

mkdir build
cd build

cmake .. \
    -DUSE_DEPTH_COMPUTE_OPENSOURCE=ON \
    -DUSE_DEPTH_COMPUTE_STUBS=OFF
    
make -j$(nproc)

sudo make install
sudo ldconfig
```

开源计算库编译完成后可以在`/usr/local/lib/`下找到几个.so文件

之后安装SDK

```
git clone --branch rel-4.2.1 --depth 1 \
    https://github.com/analogdevicesinc/ToF.git

cd ToF

mkdir build
cd build

cmake .. \
    -DWITH_EXAMPLES=ON \
    -DWITH_NETWORK=ON \
    -DUSE_DEPTH_COMPUTE_OPENSOURCE=ON \
    -DUSE_DEPTH_COMPUTE_STUBS=OFF \
    -DCMAKE_PREFIX_PATH="/opt/glog;/opt/protobuf;/opt/websockets"

make -j$(nproc)
```

注意以上libaditof和ToF都需要在make时显式指定使用开源库：

    -DUSE_DEPTH_COMPUTE_OPENSOURCE=ON \
    -DUSE_DEPTH_COMPUTE_STUBS=OFF \

或者去改CMakeList.txt，设置以上两项



## 摄像头使用

首先将摄像头接入，输入以下命令

```
lsusb
```

会有以下输出

```
linaro@linaro-alip:~/rk3588_tof/ToF/build/examples/tof-viewer$ lsusb
Bus 006 Device 001: ID 1d6b:0003 Linux Foundation 3.0 root hub
Bus 005 Device 014: ID 0456:a4a2 Analog Devices, Inc. ADI TOF USB Gadget
Bus 005 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub
Bus 004 Device 001: ID 1d6b:0001 Linux Foundation 1.1 root hub
Bus 002 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub
Bus 003 Device 001: ID 1d6b:0001 Linux Foundation 1.1 root hub
Bus 001 Device 004: ID 28a0:1185  USB OPTICAL MOUSE 
Bus 001 Device 003: ID 1a2c:2124 China Resource Semico Co., Ltd Keyboard
Bus 001 Device 002: ID 05e3:0610 Genesys Logic, Inc. Hub
Bus 001 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub
```

其中`Bus 005 Device 014: ID 0456:a4a2 Analog Devices, Inc. ADI TOF USB Gadget`是ToF摄像头设备

接下来输入

```
lsusb -t
```

会看到以下输出

```
linaro@linaro-alip:~/rk3588_tof/ToF/build/examples/tof-viewer$ lsusb -t
/:  Bus 06.Port 1: Dev 1, Class=root_hub, Driver=xhci-hcd/0p, 5000M
/:  Bus 05.Port 1: Dev 1, Class=root_hub, Driver=xhci-hcd/1p, 480M
    |__ Port 1: Dev 14, If 0, Class=Communications, Driver=rndis_host, 480M
    |__ Port 1: Dev 14, If 1, Class=CDC Data, Driver=rndis_host, 480M
    |__ Port 1: Dev 14, If 2, Class=Mass Storage, Driver=usb-storage, 480M
/:  Bus 04.Port 1: Dev 1, Class=root_hub, Driver=ohci-platform/1p, 12M
/:  Bus 03.Port 1: Dev 1, Class=root_hub, Driver=ohci-platform/1p, 12M
/:  Bus 02.Port 1: Dev 1, Class=root_hub, Driver=ehci-platform/1p, 480M
/:  Bus 01.Port 1: Dev 1, Class=root_hub, Driver=ehci-platform/1p, 480M
    |__ Port 1: Dev 2, If 0, Class=Hub, Driver=hub/4p, 480M
        |__ Port 1: Dev 3, If 0, Class=Human Interface Device, Driver=usbhid, 1.5M
        |__ Port 1: Dev 3, If 1, Class=Human Interface Device, Driver=usbhid, 1.5M
        |__ Port 2: Dev 4, If 0, Class=Human Interface Device, Driver=usbhid, 1.5M
```

其中

```
/:  Bus 05.Port 1: Dev 1, Class=root_hub, Driver=xhci-hcd/1p, 480M
    |__ Port 1: Dev 14, If 0, Class=Communications, Driver=rndis_host, 480M
    |__ Port 1: Dev 14, If 1, Class=CDC Data, Driver=rndis_host, 480M
    |__ Port 1: Dev 14, If 2, Class=Mass Storage, Driver=usb-storage, 480M
```

是ToF摄像头，但是由于缺少对应模块的原因，可能显示为

```
/: Bus 05.Port 1: Dev 1, Class=root_hub, Driver=xhci-hcd/1p, 480M 
	|__ Port 1: Dev 2, If 0, Class=Communications, Driver=, 480M 
	|__ Port 1: Dev 2, If 1, Class=CDC Data, Driver=, 480M 
	|__ Port 1: Dev 2, If 2, Class=Mass Storage, Driver=usb-storage, 480M
```

也就是只识别到一个usb存储设备，另外两个没有Driver

如果出现这种情况，要下载内核源码并编译相应模块

建议先备份目前内核已有的module以及其他需要备份的，避免出问题

还是在rk3588_tof下，执行：

```
git clone --branch develop-5.10 \
    https://github.com/rockchip-linux/kernel.git

cd kernel

git checkout e4e23512cba0fcc6548e21033180c141dd0b86c6

cp /boot/config-$(uname -r) .config

make olddefconfig

make modules_prepare

make menuconfig
```

之后会出现一个菜单，按照以下目录寻找项目操作，

```
Device Drivers
  → Network device support
    → USB Network Adapters
```

选择以下几项

```
<M> Multi-purpose USB Networking Framework
<M> CDC Ethernet support
<M> CDC NCM support
<M> Host for RNDIS devices
```

要选M而不是\*，每个项目可以按空格切换，之后保存退出

之后临时绕过跳过 modpost symbol 冲突检查，并编译模块：

```
cd ~/rk3588_tof/kernel

mv Module.symvers Module.symvers.bak

make M=drivers/net/usb modules
```

并复制生成的.ko文件：

```
sudo cp drivers/net/usb/usbnet.ko \
/lib/modules/$(uname -r)/kernel/drivers/net/usb/

sudo cp drivers/net/usb/cdc_ether.ko \
/lib/modules/$(uname -r)/kernel/drivers/net/usb/

sudo cp drivers/net/usb/rndis_host.ko \
/lib/modules/$(uname -r)/kernel/drivers/net/usb/

sudo cp drivers/net/usb/cdc_ncm.ko \
/lib/modules/$(uname -r)/kernel/drivers/net/usb/

mv Module.symvers.bak Module.symvers

sudo depmod -a

sudo modprobe usbnet
sudo modprobe cdc_ether
sudo modprobe rndis_host
sudo modprobe cdc_ncm
```

之后重新插拔摄像头应该就可以识别为：

```
/:  Bus 05.Port 1: Dev 1, Class=root_hub, Driver=xhci-hcd/1p, 480M
    |__ Port 1: Dev 14, If 0, Class=Communications, Driver=rndis_host, 480M
    |__ Port 1: Dev 14, If 1, Class=CDC Data, Driver=rndis_host, 480M
    |__ Port 1: Dev 14, If 2, Class=Mass Storage, Driver=usb-storage, 480M
```

如果成功，就安装完毕了



## SDK运行

安装完之后可以尝试运行data_collect

```
cd ~/rk3588_tof/ToF/build/examples/data_collect 
sudo ./data_collect  --ip 10.42.0.1  --m 0 config/config_crosby_adsd3500_new_modes.json
```

正常会看到以下输出：

```
linaro@linaro-alip:~/rk3588_tof/ToF/build/examples/tof-viewer$ cd ~/rk3588_tof/ToF/build/examples/data_collect 
sudo ./data_collect  --ip 10.42.0.1  --m 0 config/config_crosby_adsd3500_new_modes.json
I0521 11:26:01.301944 40583 main.cpp:144] SDK version: 4.2.0 | branch: rel-4.2.1 | commit: cb7cd73f
I0521 11:26:01.325188 40583 main.cpp:264] Output folder: ./
I0521 11:26:01.325242 40583 main.cpp:265] Mode: 0
I0521 11:26:01.325251 40583 main.cpp:266] Number of frames: 1
I0521 11:26:01.325256 40583 main.cpp:267] Json file: config/config_crosby_adsd3500_new_modes.json
I0521 11:26:01.325261 40583 main.cpp:268] Frame type is: raw
I0521 11:26:01.325266 40583 main.cpp:269] Warm Up Time is: 0 seconds
I0521 11:26:01.325271 40583 main.cpp:272] Ip address is: 10.42.0.1
I0521 11:26:01.325333 40583 system_impl.cpp:134] SDK built with websockets version:4.0.20
I0521 11:26:01.325416 40583 network_sensor_enumerator.cpp:57] Looking for sensors over network
Conn established
Connection Closed
I0521 11:26:03.334079 40583 camera_itof.cpp:146] Initializing camera
Conn established
I0521 11:26:12.353747 40583 mode_info.cpp:144] Using new mixed modes table for adsd3500.
I0521 11:26:29.371452 40583 camera_itof.cpp:355] Current adsd3500 firmware version is: 4.2.4.0
I0521 11:26:29.371706 40583 camera_itof.cpp:357] Current adsd3500 firmware git hash is: 5ecd368683f2b49f6af7c872deaf04d5696206c4
I0521 11:26:29.372936 40583 camera_itof.cpp:1766] Found Depth ini file: ./config/RawToDepthAdsd3500_lr-qnative.ini
I0521 11:26:29.373335 40583 camera_itof.cpp:1766] Found Depth ini file: ./config/RawToDepthAdsd3500_lr-native.ini
I0521 11:26:29.373579 40583 camera_itof.cpp:1766] Found Depth ini file: ./config/RawToDepthAdsd3500_sr-qnative.ini
I0521 11:26:29.373781 40583 camera_itof.cpp:1766] Found Depth ini file: ./config/RawToDepthAdsd3500_sr-native.ini
I0521 11:26:29.373940 40583 camera_itof.cpp:1766] Found Depth ini file: ./config/RawToDepthAdsd3500_sr-mixed.ini
I0521 11:26:29.374029 40583 camera_itof.cpp:1766] Found Depth ini file: ./config/RawToDepthAdsd3500_lr-mixed.ini
I0521 11:26:29.374161 40583 camera_itof.cpp:1766] Found Depth ini file: ./config/RawToDepthAdsd_pcm-native.ini
I0521 11:26:29.374310 40583 camera_itof.cpp:1777] Current Depth ini file is: ./config/RawToDepthAdsd3500_lr-mixed.ini
I0521 11:26:30.361126 40583 camera_itof.cpp:450] Camera FPS set from Json file at: 10
I0521 11:26:31.358966 40583 camera_itof.cpp:462] Camera initialized
I0521 11:26:31.359429 40583 main.cpp:315] SD card image version: microsd-4.2.0-b260ad46.img
I0521 11:26:31.359529 40583 main.cpp:316] Kernel version: lf-5.10.72-2.2.0
I0521 11:26:31.359602 40583 main.cpp:317] U-Boot version: imx_v2020.04_5.4.70_2.3.0
I0521 11:26:41.360498 40583 camera_itof.cpp:539] Chosen mode: sr-native
I0521 11:26:41.360744 40583 camera_itof.cpp:593] Using ini file: ./config/RawToDepthAdsd3500_sr-native.ini
I0521 11:26:43.704655 40583 main.cpp:452] Requesting 1 frames!
I0521 11:26:44.596174 40583 main.cpp:605] FPS: 1.12197
I0521 11:26:44.596375 40583 network_depth_sensor.cpp:186] Stopping device
Connection Closed
```

尝试运行GUI：

```
cd ~/rk3588_tof/ToF/build/examples/tof-viewer

sudo ./ADIToFGUI \
--ip 10.42.0.1 \
tof-viewer_config_adsd3500_new_modes.json
```

会看到设备自带屏幕出现GUI窗口，即可正常使用