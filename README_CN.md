# WT99P461_S1_1V1 示例项目

[English](./README.md)

## 项目简介

这是基于 **WT99P461_S1_1V1**（P4+C61 模组）的示例工程，采用 ESP-Brookesia UI 框架展示类似智能手机的界面。集成音视频播放、摄像头、游戏、计算器等应用，并支持人脸/行人检测；Wi-Fi/BLE 由模组内 C61 通过 **SPI Full-duplex** 托管提供。

## 主要特性

- 🎯 **智能手机式UI界面** - 基于ESP-Brookesia框架
- 📱 **多种应用程序** - 计算器、音乐播放器、视频播放器、2048游戏、摄像头应用
- 🤖 **AI视觉功能** - 人脸检测、行人检测
- 🖥️ **高清显示** - MIPI DSI（JD9165）+ GT911 触摸
- 🎵 **音频处理** - 支持MP3解码和音频播放（ES8311）
- 🎬 **视频处理** - 目前视频播放器支持 MJPEG
- 💾 **多存储支持** - SPIFFS + TF（SDMMC Slot1 + 模拟开关）
- 🌐 **网络连接** - Wi-Fi/BLE（Hosted SPI → C61）+ 以太网（IP101）
- 📷 **摄像头支持** - SC2336 MIPI CSI，默认 RAW8 1024×600@30fps
- 💡 **板载 RGB LED** - WS2812（GPIO54），`espressif/led_strip` 驱动

## 视频播放器功能

### 支持的视频格式

>[!NOTE]
>**视频播放说明**
>- 需要将 MJPEG 格式的视频保存到 SD 卡上，并将 SD 卡插入 SD 卡槽
>- 目前仅支持 MJPEG 格式的视频
>- 插入 SD 卡后，视频播放 APP 将自动出现在界面上

### 视频格式转换

>[!TIP]
>**视频格式转换方法**
>
>1. 安装 ffmpeg：
>```bash
>sudo apt update
>sudo apt install ffmpeg
>```
>
>2. 使用 ffmpeg 进行视频转换：
>```bash
>ffmpeg -i YOUR_INPUT_FILE_NAME.mp4 -vcodec mjpeg -q:v 2 -vf "scale=1024:600" -acodec copy YOUR_OUTPUT_FILE_NAME.mjpeg
>```

### 视频播放器使用方法

1. **准备视频文件**
   - 使用上述方法将视频文件转换为 MJPEG 格式
   - 确保分辨率调整为 1024x600 以获得最佳显示效果

2. **设置SD卡**
   - 将格式化的 SD 卡插入 SD 卡槽
   - 将转换后的 MJPEG 文件复制到 SD 卡中

3. **启动视频播放器**
   - 视频播放器应用会自动读取 SD 卡
   - 从读取的文件列表中选择视频进行播放

## 环境搭建

### 硬件要求

- **开发板**: WT99P461_S1_1V1（ESP32-P4 主机 + 模组内 ESP32-C61）

![WT99P461_S1_1V1](./docs/WT99P461_S1_1V1.png#pic_center)

- **显示屏**: MIPI DSI（JD9165）+ 触摸（GT911）
- **存储**: TF 卡（可选；与 Hosted SPI 分总线，经 GPIO33 开关选通）
- **摄像头**: SC2336（MIPI CSI，默认 RAW8 1024×600、30 fps）
- **喇叭**: 3W
- **RGB LED**: 板载 WS2812，数据脚 GPIO54


### 软件环境搭建

#### 1. 安装ESP-IDF

请按照官方文档安装ESP-IDF v6.0.1：
- [ESP-IDF 入门指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/index.html)

#### 2. 克隆项目

```bash
git clone <项目仓库地址>
cd WT99P461_S1_1V1
```

#### 3. 配置环境变量

```bash
. ${IDF_PATH}/export.sh
```

#### 4. 安装项目依赖

```bash
# 安装管理的组件依赖
idf.py reconfigure
```

#### 5. 编译和烧录

```bash
# 配置项目
idf.py menuconfig

# 编译项目
idf.py build

# 烧录到设备
idf.py flash

# 查看串口输出
idf.py monitor
```

## WIFI/BLE使用

### 说明

- ESP32-P4 本身不带 Wi-Fi/BLE，本板由模组内 **ESP32-C61** 作为 ESP-Hosted 从机提供无线能力。
- **默认传输为 SPI Full-duplex**（复用模组原 SDIO 焊线脚序），**不是 SDIO**。主机默认配置见 `sdkconfig.defaults`。
- **从机复位脚为 GPIO13**（连 C61 `CHIP_PU/EN`）。

### Hosted SPI 引脚（本板默认）

| 信号 | P4（主机） | C61（从机） |
|------|-----------|-------------|
| MOSI | GPIO14 | GPIO27 |
| MISO | GPIO15 | GPIO28 |
| CLK  | GPIO18 | GPIO26 |
| CS   | GPIO19 | GPIO25 |
| HS   | GPIO16 | GPIO22 |
| DR   | GPIO17 | GPIO23 |
| RST  | GPIO13 | EN / CHIP_PU |

SPI Mode **3**。C61 的 GPIO20/21 属于 Flash，不可作传输脚。

### 使用

#### ESP32-P4 主机工程配置

##### 确认从机目标为 ESP32-C61

```bash
(Top) → Component config → Wi-Fi Remote → choose slave target
```

默认应为 `ESP32C61`。传输与复位引脚已由 `sdkconfig.defaults` 固定为上表 SPI 映射.

## 工程目录介绍

```
WT99P461_S1_1V1/
├── main/                           # 主程序（P4 主机）
│   ├── main.cpp
│   ├── CMakeLists.txt
│   └── idf_component.yml           # 含 esp_hosted / led_strip 等
├── components/
│   ├── apps/                       # 计算器/摄像头/音乐/视频/2048/设置
│   ├── human_face_detect/
│   ├── pedestrian_detect/
│   ├── wt99p461_s1_board/          # 板级 BSP（含 WS2812 API）
│   ├── bsp_extra/
│   └── esp_lcd_jd9165/             # 本板 LCD 驱动（本地组件）
├── managed_components/             # 组件管理器下载（勿手改）
├── tools/                          # C61 slave 构建/烧录脚本等
├── firmware/                       # 预编译主机/从机固件包
├── spiffs/
├── sdkconfig.defaults              # 含 Hosted SPI 与 GPIO13 复位
├── partitions.csv
└── README_CN.md
```

### 核心组件说明

#### 1. 主程序 (`main/`)
- **main.cpp**: 程序入口，初始化系统、显示、存储、网络等模块，并启动各个应用程序

#### 2. 应用程序 (`components/apps/`)
- **calculator/**: 计算器应用，支持基本四则运算
- **camera/**: 摄像头应用，支持拍照和视频预览
- **game_2048/**: 经典2048数字游戏
- **music_player/**: 音乐播放器，支持MP3格式
- **setting/**: 系统设置应用
- **video_player/**: 视频播放器，支持H.264格式

#### 3. AI视觉组件
- **human_face_detect/**: 人脸检测算法实现
- **pedestrian_detect/**: 行人检测算法实现

#### 4. 硬件抽象层
- **wt99p461_s1_board/**: 开发板专用 BSP（显示/触摸/音频/TF/以太网/WS2812 等）
- **bsp_extra/**: 扩展的 BSP 功能模块
- **esp_lcd_jd9165/**: JD9165 MIPI DSI 面板驱动

#### 5. 存储和资源
- **spiffs/**: 内置文件系统，存储应用资源和配置文件
- **mp4/**: 视频文件存储目录

## 分区配置

>[!INFO]
>**自定义分区表说明 (`partitions.csv`)**
>- **nvs** (24KB): 非易失性存储，用于配置数据
>- **phy_init** (4KB): RF校准数据
>- **factory** (9MB): 应用程序固件
>- **storage** (4MB): SPIFFS文件系统

## 开发调试

### 查看日志
```bash
idf.py monitor
```

>[!INFO]
>**性能监控说明**
>程序内置内存监控功能，每5秒输出一次内存使用情况：
>- SRAM使用状态
>- PSRAM使用状态  
>- 内存泄漏警告

### 常用配置项
通过 `idf.py menuconfig` 可以配置：
- 显示屏参数设置
- 摄像头分辨率配置
- 音频采样率设置
- Wi-Fi和以太网配置

## 组件库版本要求

所有直接依赖均在各组件的 `idf_component.yml` 中固定为精确版本；间接依赖由仓库中的 `dependencies.lock` 固定。不要删除锁文件或修改 `managed_components`。

| 组件 | 固定版本 |
|---|---|
| `chmorgan/esp-audio-player` | `1.1.0` |
| `chmorgan/esp-file-iterator` | `1.0.0` |
| `chmorgan/esp-libhelix-mp3` | `1.0.3` |
| `espressif/cmake_utilities` | `0.5.3` |
| `espressif/dl_fft` | `0.7.0` |
| `espressif/eppp_link` | `1.1.6` |
| `espressif/esp-brookesia` | `0.4.2` |
| `espressif/esp-dl` | `3.3.8` |
| `espressif/esp_cam_sensor` | `2.1.0` |
| `espressif/esp_codec_dev` | `1.5.11` |
| `espressif/esp_h264` | `1.3.0` |
| `espressif/esp_hosted` | `2.12.13` |
| `espressif/esp_ipa` | `2.0.0` |
| `espressif/esp_lcd_touch` | `1.2.1` |
| `espressif/esp_lcd_touch_gt911` | `1.2.1` |
| `espressif/esp_lvgl_port` | `2.8.0~1` |
| `espressif/esp_new_jpeg` | `1.0.2` |
| `espressif/esp_sccb_intf` | `0.0.9` |
| `espressif/esp_serial_slave_link` | `1.1.2` |
| `espressif/esp_video` | `2.1.0` |
| `espressif/esp_wifi_remote` | `1.6.4` |
| `espressif/ip101` | `1.0.0` |
| `espressif/led_strip` | `3.0.3` |
| `espressif/usb` | `1.5.0` |
| `espressif/usb_host_uvc` | `2.4.2` |
| `espressif/wifi_remote_over_eppp` | `0.3.3` |
| `idf` | `6.0.1` |
| `lvgl/lvgl` | `8.4.0` |

本板 LCD 使用本地组件 `components/esp_lcd_jd9165`（非 `esp_lcd_ek79007`）。

激活 IDF 6.0.1 环境并编译后，可运行 `python tools/verify_dependencies.py` 检查组件内容是否与锁文件一致。详见 [迁移说明](docs/IDF6_MIGRATION.md)。

## 日志输出

```bash
I (25) boot: ESP-IDF v6.0.1-dirty 2nd stage bootloader
I (26) boot: compile time Sep 28 2026 15:03:14
I (26) boot: Multicore bootloader
I (28) boot: chip revision: v3.2
I (29) boot: efuse block revision: v1.2
I (33) qio_mode: Enabling default flash chip QIO
I (37) boot.esp32p4: SPI Speed      : 80MHz
I (41) boot.esp32p4: SPI Mode       : QIO
I (45) boot.esp32p4: SPI Flash Size : 16MB
I (49) boot: Enabling RNG early entropy source...
I (53) boot: Partition Table:
I (56) boot: ## Label            Usage          Type ST Offset   Length
I (62) boot:  0 nvs              WiFi data        01 02 00011000 00006000
I (69) boot:  1 phy_init         RF data          01 01 00017000 00001000
I (75) boot:  2 factory          factory app      00 00 00020000 00900000
I (82) boot:  3 storage          Unknown data     01 82 00920000 00400000
I (89) boot: End of partition table
I (92) esp_image: segment 0: paddr=00020020 vaddr=48360020 size=444c60h (4476000) map
I (697) esp_image: segment 1: paddr=00464c88 vaddr=30100000 size=00204h (   516) load
I (698) esp_image: segment 2: paddr=00464e94 vaddr=30100210 size=00048h (    72) load
I (702) esp_image: segment 3: paddr=00464ee4 vaddr=4ff40000 size=0b134h ( 45364) load
I (716) esp_image: segment 4: paddr=00470020 vaddr=48000020 size=354ce0h (3493088) map
I (1183) esp_image: segment 5: paddr=007c4d08 vaddr=4ff4b134 size=1970ch (104204) load
I (1200) esp_image: segment 6: paddr=007de41c vaddr=4ff64880 size=05b90h ( 23440) load
I (1205) esp_image: segment 7: paddr=007e3fb4 vaddr=50108080 size=00024h (    36) load
I (1214) boot: Loaded app from partition at offset 0x20000
I (1214) boot: Disabling RNG early entropy source...
I (1227) hex_psram: vendor id    : 0x0d (AP)
I (1227) hex_psram: Latency      : 0x01 (Fixed)
I (1228) hex_psram: DriveStr.    : 0x00 (25 Ohm)
I (1229) hex_psram: dev id       : 0x03 (generation 4)
I (1233) hex_psram: density      : 0x07 (256 Mbit)
I (1238) hex_psram: good-die     : 0x06 (Pass)
I (1242) hex_psram: SRF          : 0x02 (Slow Refresh)
I (1247) hex_psram: BurstType    : 0x00 ( Wrap)
I (1251) hex_psram: BurstLen     : 0x03 (2048 Byte)
I (1256) hex_psram: BitMode      : 0x01 (X16 Mode)
I (1260) hex_psram: Readlatency  : 0x04 (14 cycles@Fixed)
I (1265) hex_psram: DriveStrength: 0x00 (1/1)
I (1270) MSPI Timing: Enter psram timing tuning
I esp_psram: Found 32MB PSRAM device
I esp_psram: Speed: 200MHz
I (1634) mmu_psram: .rodata xip on psram
I (1794) mmu_psram: .text xip on psram
I (1795) hex_psram: psram CS IO is dedicated
I (1795) cpu_start: Multicore app
I (2136) esp_psram: SPI SRAM memory test OK
I (2145) cpu_start: GPIO 38 and 37 are used as console UART I/O pins
I (2145) cpu_start: Pro cpu start user code
I (2146) cpu_start: cpu freq: 400000000 Hz
I (2148) app_init: Application information:
I (2152) app_init: Project name:     wt99p461_s1_board
I (2156) app_init: App version:      bb70557-dirty
I (2161) app_init: Compile time:     Sep 28 2026 15:03:18
I (2166) app_init: ELF file SHA256:  322b73755...
I (2170) app_init: ESP-IDF:          v6.0.1-dirty
I (2175) efuse_init: Min chip rev:     v3.1
I (2179) efuse_init: Max chip rev:     v3.99 
I (2183) efuse_init: Chip rev:         v3.2
I (2187) heap_init: Initializing. RAM available for dynamic allocation:
I (2193) heap_init: At 4FF6FB80 len 0004B440 (301 KiB): RETENT_RAM
I (2199) heap_init: At 4FFBAFC0 len 00004BF0 (18 KiB): RAM
I (2204) heap_init: At 501080A4 len 00007F5C (31 KiB): RTCRAM
I (2210) heap_init: At 30100258 len 00001DA8 (7 KiB): SPM
I (2215) esp_psram: Adding pool of 24896K of PSRAM memory to heap allocator
I (2222) esp_psram: Adding pool of 44K of PSRAM memory gap generated due to end address alignment of irom to the heap allocator
I (2233) esp_psram: Adding pool of 44K of PSRAM memory gap generated due to end address alignment of drom to the heap allocator
I (2244) spi_flash: detected chip: generic
I (2248) spi_flash: flash io: qio
I (2251) host_init: ESP Hosted : Host chip_ip[18]
I (2255) H_API: ESP-Hosted starting. Hosted_Tasks: prio:23, stack: 5120 RPC_task_stack: 5120
I (2263) H_API: ** add_esp_wifi_remote_channels **
I (2268) spi_wrapper: Transport: SPI, Mode:3 Freq:10MHz TxQ:15 RxQ:15
 GPIOs: CLK:18 MOSI:14 MISO:15 CS:19 HS:16 DR:17 SlaveReset:13
I (2280) sleep_gpio: Configure to isolate all GPIO pins in sleep state
I (2286) sleep_gpio: Enable automatic switching of GPIO sleep configuration
I (2293) spi: Staring SPI task
I (2296) os_wrapper_esp: GPIO [16] configuring as Interrupt
I (2301) main_task: Started on CPU0
I (2301) os_wrapper_esp: GPIO [17] configuring as Interrupt
I (2310) main_task: Calling app_main()
I (2389) wt99p461_s1_board: Partition size: total: 3848081, used: 3217067
I (2389) app_main: SPIFFS mount successfully
I (2389) wt99p461_s1_board: TF SDMMC Slot1+SWITCH 4-bit (CLK=44 CMD=41 D0=42 D1=43 D2=39 D3=40)
I (2497) wt99p461_s1_board: LDO4 configured 3300 mV; card VDD is a separate measured node
I (2635) wt99p461_s1_board: TF mounted OK (SDMMC 4-bit@20000kHz)
I (2635) app_main: SD card mount successfully
I (2641) ES8311: Work in Slave mode
I (2648) ES8311: Work in Slave mode
I (2651) I2S_IF: Paired data: 0x4ffbfa44, current mode: playback, paired in_enable: 0, paired out_enable: 0
I (2651) I2S_IF: STD: TX, data_bit: 16, slot_bit: 16, ws_width: 16, slot_mode: STEREO, slot_mask: 0x3
I (2659) I2S_IF: STD: TX, sample_rate_hz: 16000, mclk_multiple: 256, clk_src: 0
I (2680) Adev_Codec: Open codec device OK
I (2681) I2S_IF: Current mode(record) and peer mode have same sample_rate 16000, channel 2, bits_per_sample 16
I (2683) I2S_IF: STD: RX, data_bit: 16, slot_bit: 16, ws_width: 16, slot_mode: STEREO, slot_mask: 0x3
I (2692) I2S_IF: STD: RX, sample_rate_hz: 16000, mclk_multiple: 256, clk_src: 0
I (2711) Adev_Codec: Open codec device OK
I (2712) app_main: Codec init successfully
I (2712) wt99p461_s1_board: WS2812 ready on GPIO54
I (2713) app_main: RGB LED white @10% (RGB=25)
I (2717) LVGL: Starting LVGL task
I (2791) GT911: TouchPad_ID:0x39,0x31,0x31
I (2791) GT911: TouchPad_Config_Version:65
I (2791) wt99p461_s1_board: MIPI DSI PHY Powered on
I (2792) wt99p461_s1_board: LCD scan dir: UPDN=26 SHLR=27 (GPIO high → panel scan flipped 180°)
I (2802) wt99p461_s1_board: Install MIPI DSI LCD control panel
I (2806) wt99p461_s1_board: Install JD9165BA LCD control panel
I (2825) jd9165: Software reset DSI panel
I (3085) wt99p461_s1_board: Display initialized: JD9165BA 1024x600, lanes=2, lane_rate=600 Mbps; shared reset GPIO=23 active-high
I (3085) wt99p461_s1_board: Display resolution 1024x600
I (3091) wt99p461_s1_board: Setting LCD backlight: 100%
I (3096) app_main: Display ESP-Brookesia phone demo
[WARN] [esp_brookesia_core.cpp:46](getDisplaySize): Display is not set, use default display
[INFO] [esp_brookesia_core.cpp:204](beginCore): Library version: 0.4.1
I (3136) esp_eth.netif.netif_glue: e8:f6:0a:e7:cc:54
I (3136) esp_eth.netif.netif_glue: ethernet attached to netif
[WARN] [esp_brookesia_phone_manager.cpp:72](begin): No touch device is set, try to use default touch device
[WARN] [esp_brookesia_phone_manager.cpp:76](begin): Using default touch device(@0x0x487adf34)
I (3199) file_iterator: File : For Elise.mp3
I (3205) file_iterator: File : Waka Waka.mp3
I (3213) file_iterator: File : BGM 1.mp3
I (3219) file_iterator: File : Something.mp3
I (3224) file_iterator: File : BGM 2.mp3
I (3225) EUI_Setting: Load ble_en: 0
I (3225) EUI_Setting: Load brightness: 27
I (3225) EUI_Setting: Load volume: 5
I (3227) EUI_Setting: Load wifi_en: 1
I (3231) bsp_extra_board: Setting volume: 5
I (3234) wt99p461_s1_board: Setting LCD backlight: 27%
I (3240) transport: Attempt connection with slave: retry[0]
I (3245) spi: Resetting slave
I (3247) spi: Resetting slave on SPI bus with pin 13
I (3252) os_wrapper_esp: GPIO [13] configured
I (3283) file_iterator: File : excellent.mp3
I (3283) file_iterator: File : normal.mp3
I (3283) file_iterator: File : weak.mp3
I (3284) file_iterator: File : good.mp3
I (3287) Game2048: Load score: 52
I (3292) MEM:    Biggest /     Free /    Total
          SRAM : [40 / 79 / 359] KB
         PSRAM : [22016 / 22536 / 24985] KB
I (3776) transport: Waiting for esp_hosted slave to be ready
I (3826) transport: Received INIT event from ESP32 peripheral
I (3826) transport: EVENT: 12
I (3826) transport: Identified slave [esp32c61]
I (3827) transport: EVENT: 11
I (3830) transport: capabilities: 0xe8
I (3834) transport: Features supported are:
I (3837) transport:        - HCI over SPI
I (3841) transport:        - BLE only
I (3844) transport: EVENT: 16
I (3847) transport: extended capabilities: 0x0
I (3851) transport: Extended Features supported: [0]
I (3856) transport: EVENT: 13
I (3858) transport: ESP board type is : 20 

I (3862) transport: Base transport is set-up, TRANSPORT_TX_ACTIVE
I (3868) H_API: Transport active
I (3871) transport: Slave chip Id[12]
I (3875) transport: raw_tp_dir[-], flow_ctrl: low[60] high[90]
I (3880) transport: transport_delayed_init
I (3884) esp_cli: Registering command: crash
I (3888) esp_cli: Registering command: reboot
I (3892) esp_cli: Registering command: mem-dump
I (3896) esp_cli: Registering command: task-dump
I (3901) esp_cli: Registering command: cpu-dump
I (3905) esp_cli: Registering command: heap-trace
I (3909) esp_cli: Registering command: sock-dump
I (3914) esp_cli: Registering command: host-power-save
I (3919) hci_stub_drv: Host BT Support: Disabled
I (3923) spi: Received INIT event
I (4293) RPC_WRAP: Coprocessor Boot-up
I (4471) EUI_Setting: ESP-Hosted slave FW 2.12.13
I (4569) EUI_Setting: wifi_init done
I (4571) RPC_WRAP: ESP Event: wifi station started
I (4605) RPC_WRAP: ESP Event: wifi station started
I (7137) wt99p461_s1_board: Ethernet Started
I (7137) app_main: Ethernet init successfully
I (8328) MEM:    Biggest /     Free /    Total
          SRAM : [40 / 83 / 359] KB
         PSRAM : [22016 / 22536 / 24985] KB
I (13328) MEM:    Biggest /     Free /    Total
          SRAM : [40 / 83 / 359] KB
         PSRAM : [22016 / 22536 / 24985] KB
```
