# Badge · 端侧情绪价值应用集

面向 **Waveshare ESP32-S3-Touch-AMOLED-1.75** 圆形触屏开发板的双应用固件：

1. **Grokbot（grokbot，默认）**：离线播放全部 53 个预烘焙动画状态，支持触摸、IMU、麦克风和合成音效。
2. **猫咪 GIF（gif）**：循环播放 6 个猫咪情绪 GIF，支持触摸、IMU、麦克风和合成音效。

**BOOT 单击**在 GIF ↔ Grokbot 之间切换；**BOOT 长按 2 秒**开关屏幕 debug overlay；**PWR 单击**在 Grokbot 中切换下一个表情、在 GIF 中切换下一张。全部逻辑运行在设备端，**不依赖云端、LLM、账号或网络**。

## 1. 硬件平台

| 项目 | 规格 |
| --- | --- |
| 主控 | ESP32-S3R8，双核 Xtensa LX7 @ 240MHz |
| 内存 | 8MB Octal PSRAM（80MHz）+ 内部 SRAM |
| Flash | 物理 32MB，QIO @ 80MHz |
| 屏幕 | 1.75" 圆形 AMOLED，466×466，RGB565，CO5300，QSPI |
| 触摸 | CST9217 电容触摸，I2C |
| IMU | QMI8658 6 轴，I2C，本项目使用加速度 |
| 音频 | ES8311 扬声器 + ES7210 双通道麦克风，I2S 24kHz/16bit |
| 调试串口 | USB-Serial/JTAG，UART0 GPIO44/43 |

关键引脚：

- LCD QSPI：CS=12，PCLK=38，D0=4，D1=5，D2=6，D3=7，RST=39
- I2C：SCL=14，SDA=15
- I2S：MCLK=42，SCLK=9，LCLK=45，DOUT=8，DSIN=10，PA=46
- 触摸：RST=40，INT=11
- BOOT：GPIO0，单击切应用 / 长按 2 秒切 Debug
- PWR：AXP2101（I2C 0x34）短按中断，Grokbot 切下一表情 / GIF 切下一张

## 2. 应用与交互

### 2.1 Grokbot（默认）

- 全部 53 个状态：`idle / listening / thinking / streaming / searching / reading / working / coding / running / computing / controlling / waiting / blocked / finished / failed / sleeping / waking / happy / excited / laughing / proud / curious / playful / shy / surprised / suspicious / angry / confused / sad / drowsy / bored / scared / celebrate / approving / orbit / radar / progress / spawning / loading / dictating / sending / receiving / uploading / writing / alerting / bouncing / powering-down / humming / notifying / dragging / logo / showcase / turntable`
- 源图 400×400、30FPS、4bit 灰度，离线双线性插值预缩放为 440×440，运行时直接绘制，居中显示在 466×466 圆屏
- GBZ1 格式：zlib 压缩 + XOR 差分；切状态从关键帧重建，避免残影
- 图集保留在 SPIFFS，按当前表情读取最大约 481KB 到 PSRAM，不加载完整 11.5MB 图集
- 轻点下一状态、左滑下一/右滑上一、双击随机
- PWR 单击下一状态
- 摇晃/轻拍/吹气推进、拍手随机、说话进入 listening、明显倾斜进入 curious
- 拿起暂停、放下恢复，并复用 boop/giggle/yawn 音效

### 2.2 猫咪 GIF

- 6 个 466×466 GIF：`idle / happy / playful / sad / shock / sleepy`
- SPIFFS 流式解码，AnimatedGIF COOKED 模式直接满屏刷新
- 左右滑动切换、轻点暂停/恢复、双击随机
- PWR 单击下一张
- 摇晃/吹气切换、拍手随机、拿起暂停/放下恢复、轻拍播放 boop 音

### 2.3 统一交互层

触摸、IMU、麦克风和音效任务开机常开，两个应用消费同一事件队列。应用切换时清空旧事件，避免输入串扰。

Debug overlay 位于屏幕中部，显示事件、麦克风 RMS/ZCR、IMU 数据、ROLL/PITCH；Grokbot 额外显示当前状态、FPS、解码耗时和空闲 PSRAM。

## 3. 编译、烧录与监视

环境：ESP-IDF v5.5.x（验证于 v5.5.2），目标 `esp32s3`。Windows + Git Bash 使用仓库根目录的 `build.bat`。

```bash
cmd //c "build.bat build"
cmd //c "build.bat -p COM16 flash"       # 完整烧录，包含 storage.bin
cmd //c "build.bat -p COM16 monitor"
```

仅修改应用代码时可加快烧录：

```bash
cmd //c "build.bat build"
cmd //c "build.bat -p COM16 app-flash"
```

- `idf.py flash` / `app-flash` 已自动执行 `hard_reset`，无需手动断电重启。
- CO5300 对暖复位敏感：POWERON、USB_UART、看门狗和异常复位均使用保守初始化（600ms sleep-out、500ms 预等待、Deep-Standby 电源循环和额外重初始化）；仅软件复位/深睡唤醒使用快速时序。
- 改动 `media/pet` 后必须完整烧录，因为 GIF 与 Grokbot 图集都在 `storage` 分区。
- 烧录前先停止串口监视器，避免 COM 端口占用。

## 4. 技术架构

```text
main/main.c
  NVS → I2C → interaction（常开）→ app_framework
  register: grokbot_app → gif_app

app_framework
  BOOT 单击：stop 当前应用 → start 下一个
  BOOT 长按 2s：开关 debug overlay
  启动失败：恢复原应用

gif_app ──┐
          ├─ display（CO5300 / 触摸 / debug overlay / DMA 条带）
grokbot_app ──┘
  │
  ├─ storage（共享 SPIFFS 挂载）
  ├─ interaction（触摸 / IMU / MIC / SFX）
  ├─ animated_gif（GIF 解码）
  └─ grokbot_player（GBZ1 流式读取 / zlib / XOR）
```

### 4.1 显示

- 自建 CO5300 QSPI 面板，复用 `esp_lcd_co5300`。
- 先初始化，再通过 `disp_sleep` 做 Deep-Standby 电源循环，清除 AMOLED 模拟域锁死状态。
- PSRAM 帧缓冲不能直接 DMA；`display_flush_rect()` 使用内部 RAM、64 字节对齐的 128 行 DMA 条带，并完成 RGB565 字节序转换。
- Grokbot 首帧全屏刷新，后续按 packed 帧字节差计算脏矩形，只绘制并刷新变化区域；CO5300 局部刷新坐标按 2 像素向外对齐，避免奇偶坐标错位产生细条纹。整帧 QSPI 颜色传输从 30 次降至 4 次，局部更新通常更少。开启 debug overlay 时仍全屏刷新。
- 首帧写入 GRAM 后才 `display_on(true)`，避免开机彩闪。

### 4.2 Grokbot 图集与解码

- 固定来源：`Ocean-lhy/Stopwatch-GrokBot@9d917f8`
- 完整图集工具：`tool/grokbot_atlas.py`（需要 Python 3、NumPy、Pillow）
- 输出：`media/pet/grokbot.atlas`
- 大小：11,482,405B
- SHA-256：`570628a71df7ae76a2c00d8327cc078086c6f2ebf49888222e90091747e87d23`
- 格式：小端 GBZ1 v1，16B 全局头、32B clip 头、8B 帧索引、8B 帧记录头
- 每帧 440×440/2=96,800B，高 nibble 优先；索引 `i` 映射为灰度 `i×17`
- 解码使用 `espressif/zlib` 的 `uncompress()`，不依赖 ROM miniz
- 正向帧 XOR 还原；倒放对当前帧再次 XOR；切状态从第 0 关键帧重建到 start frame 2
- 启动时读取全局头和 53 个 clip 头；切状态时按 offset/size 从 SPIFFS 读取当前 clip，最大 492,173B
- packed/delta 各 96.8KB，上一帧 packed 缓冲 96.8KB，显示帧约 434KB；播放任务 core1、优先级 6、栈 16KB
- 性能日志分别报告 zlib 解码、脏区检测/绘制和 QSPI 刷新耗时，便于定位帧率瓶颈

### 4.3 任务

| 任务 | 所属 | 核心 | 优先级 | 栈 | 说明 |
| --- | --- | --- | --- | --- | --- |
| `sensors` | interaction | 0 | 5 | 6144 | 触摸 + IMU + PWR，10ms 周期 |
| `mic` | interaction | 0 | 5 | 6144 | 麦克风 RMS/ZCR |
| `sfx` | interaction | 任意 | 4 | 4096 | 合成音效 |
| `gif_play` | gif | 1 | 6 | 8192 | GIF 解码与刷新 |
| `gif_input` | gif | 0 | 5 | 4096 | GIF 输入映射 |
| `grok_play` | grokbot | 1 | 6 | 16384 | GBZ1 解码、渲染、输入和性能报告 |
| `app_btn` | framework | 任意 | 3 | 4096 | BOOT 单击切应用 / 长按切 Debug |

Grokbot 播放循环每帧至少 `vTaskDelay(1)`，包括亚毫秒等待和丢帧追赶，避免 CPU1 idle task 饥饿触发看门狗。

## 5. 存储与资源

`partitions.csv`：

| 名称 | 类型 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24K | 系统持久化 |
| phy_init | data/phy | 0xf000 | 4K | RF 校准 |
| factory | app | 0x10000 | 2M | 应用固件 |
| storage | data/spiffs | 0x210000 | 0x1DF0000 | GIF + 完整 Grokbot 图集 |

- SPIFFS 使用 512B 页，避免 30MB 分区的 u16 页索引溢出。
- 媒体原始总大小约 26,866,103B；完整图集不占用 PSRAM，切换时仅加载当前状态。
- `media/pet` 同时作为 SPIFFS 打包目录；`media/pet_orig` 是 GIF 原图备份，不参与打包。

## 6. 工程结构

```text
badge/
├─ CMakeLists.txt
├─ build.bat
├─ partitions.csv
├─ sdkconfig.defaults
├─ media/
│  ├─ pet/                 # 6 个猫咪 GIF + grokbot.atlas
│  └─ pet_orig/            # GIF 原图备份
├─ tool/grokbot_atlas.py
├─ main/main.c
└─ components/
   ├─ app_framework/
   ├─ display/
   ├─ storage/
   ├─ interaction/
   ├─ animated_gif/
   ├─ gif_app/
   ├─ grokbot_app/
   └─ grokbot_player/
```

## 7. 开发约定

- 入口保持 `void app_main(void)`，日志使用 `ESP_LOG*`，显式处理 `esp_err_t`。
- 可复用功能放组件，公共头放 `include/`，依赖用 `REQUIRES` / `PRIV_REQUIRES` 声明。
- 新应用实现 `app_t{name,start,stop}`，复用 `display`、`storage`、`interaction`，在 `main.c` 注册。
- 不编辑 `build/`、`managed_components/`、`sdkconfig`；默认配置写 `sdkconfig.defaults`。
- 不提交构建产物、临时日志或凭据。

## 8. 参考与素材边界

- Waveshare 官方文档：https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75
- Waveshare 官方例程：https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75
- AnimatedGIF：https://github.com/bitbank2/AnimatedGIF （Apache-2.0）
- Grokbot 原理站：https://grokbot.yin.wiki/
- Grokbot 固件参考：https://github.com/Ocean-lhy/Stopwatch-GrokBot

参考仓库未提供 LICENSE。Grokbot 角色和图集仅用于本机个人移植，不随项目公开再分发；`tool/grokbot_atlas.py` 与本节保留来源和哈希提示。
