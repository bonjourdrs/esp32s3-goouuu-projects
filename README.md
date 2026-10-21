# ESP32-S3 GOOUUU 果云版 · 硬件验证与 AI 助手工作区

ESP32-S3（N16R8，16 MB Flash + 8 MB Octal PSRAM）开发板上的一套完整材料：

- **01–09**：九个独立的 ESP-IDF 示例工程，从单外设验证逐步走到多模块联动；
- **xiaozhi-esp32**：完整的 AI 随身助手应用（小智 v2.4.2 基础上扩展）；
- **esp-ml307-src**：`78/esp-ml307` 组件源码的本地副本，供阅读和调试。

前九个工程各自独立编译、烧录、互不依赖；后两个目录各自保留独立的 Git 仓库，父仓库通过 `.gitignore` 排除，避免嵌套提交。

## 工程总览

### 硬件验证示例

| 目录 | 功能 | 外设 | 引脚 |
|---|---|---|---|
| [`01_blink_led`](01_blink_led) | 板载 RGB 灯循环变色 + 心跳日志 | WS2812（RMT） | GPIO48 |
| [`02_speaker_tone`](02_speaker_tone) | 每 2 秒播放 do-re-mi-sol-do 音阶 | MAX98357（I2S TX） | BCLK=15 / LRC=16 / DIN=7 |
| [`03_mic_level`](03_mic_level) | 麦克风音量 → RGB 灯亮度 | INMP441（I2S RX）+ WS2812 | WS=4 / SCK=5 / SD=6 |
| [`04_mpu6050_sound`](04_mpu6050_sound) | 摇晃越强，音调越高、音量越大，静止淡出 | MPU6050（I2C）+ MAX98357 | SDA=8 / SCL=9；BCLK=15 / LRC=16 / DIN=7 |
| [`05_clap_echo`](05_clap_echo) | 拍手触发回声，倾斜板子换音高，灯随音量闪 | INMP441 + MPU6050 + MAX98357 + WS2812 | WS=4 / SCK=5 / SD=6；SDA=8 / SCL=9；BCLK=15 / LRC=16 / DIN=7 |
| [`06_offline_pet`](06_offline_pet) | 完全离线桌面宠物：说话/拿起/敲击/猛摇/倒扣各有回应，情绪会累积 | INMP441 + MPU6050 + MAX98357 + WS2812 | WS=4 / SCK=5 / SD=6；SDA=8 / SCL=9；BCLK=15 / LRC=16 / DIN=7 |
| [`07_env_sensor`](07_env_sensor) | 温湿度与气压读数，灯随温度变色 | AHT20 + BMP280（I2C）+ WS2812 | SDA=8 / SCL=9；GPIO48 |
| [`08_st7789_lcd`](08_st7789_lcd) | 240×240 彩条、文字、动态方块与帧计数 | ST7789（SPI） | SCK=21 / MOSI=47 / RST=45 / DC=40 / CS=41 / BL=42 |
| [`09_ml307_probe`](09_ml307_probe) | 识别 ML307R 4G 模组、读型号/IMEI，无 SIM 时按预期停在无卡状态 | ML307R（UART + UHCI DMA） | ESP TX=12 / ESP RX=11 |

### 应用工程

| 目录 | 说明 | 构建入口 |
|---|---|---|
| [`xiaozhi-esp32`](xiaozhi-esp32) | AI 随身助手：WiFi 优先 / 4G 自动兜底、语音对话、温湿度气压与六轴查询、摇一摇分级反馈、240×240 状态界面、可选 GPS 变体 | `python scripts/build.py goouuu-s3-dual` |
| [`esp-ml307-src`](esp-ml307-src) | `78/esp-ml307` 3.8.1 源码本地副本，用于阅读 AT 通道实现；09 工程仍通过 `idf_component.yml` 拉取同一组件 | 作为组件被引用，不单独编译 |

`xiaozhi-esp32` 的板型为 `goouuu-s3-dual`，功能和构建细节见
[xiaozhi-esp32/main/boards/goouuu-s3-dual/README.md](xiaozhi-esp32/main/boards/goouuu-s3-dual/README.md)。

## 两套工程的关系

- **01–09 是验证工程**：每个只关注一到两个外设，用来确认接线和驱动，编译产物统一放在 `D:\esp32\build\<工程名>`。
- **xiaozhi-esp32 是集成应用**：把屏幕、麦克风、功放、传感器、WiFi、4G 和云端对话整合在一起，在它上面继续做产品功能。
- **esp-ml307-src 是参考源码**：09 和 xiaozhi 使用的 4G 能力都来自这个组件，遇到 AT 指令问题时可以直接对照源码。

新增硬件功能时，先在 01–09 里单独验证，再合入 xiaozhi，避免在完整应用里排查外设问题。

## 硬件

- 核心板：ESP32-S3-DevKitC-1 42Pin（果云版），N16R8 模组（16 MB Flash / 8 MB Octal PSRAM）
- 底板：2×21P 母座，板载 WS2812 RGB LED、MAX98357 I2S 功放、INMP441 数字麦克风、AHT20、BMP280、MPU6050、ML307R 4G 模组、8P 屏幕座
- 详细引脚与模块说明见 [docs/board.md](docs/board.md)

## 开发环境

- ESP-IDF **v5.5.6**（`D:\Espressif\esp-idf`）
- 工具链 xtensa-esp32s3-elf，Python 3.10.11
- 完整环境说明见 [docs/environment.md](docs/environment.md)

## 知识库

接手这套工程前先看 [knowledge/README.md](knowledge/README.md)，里面按阅读顺序整理了从零上手到排错的全套材料：

| 文档 | 内容 |
|---|---|
| [01-quickstart.md](knowledge/01-quickstart.md) | 三步编译烧录并验证 |
| [02-toolchain-setup.md](knowledge/02-toolchain-setup.md) | 从零搭建工具链，含国内镜像加速 |
| [03-hardware.md](knowledge/03-hardware.md) | 引脚、接线与上电注意事项 |
| [04-architecture.md](knowledge/04-architecture.md) | 模块划分、音频数据流与仓库分层 |
| [05-workflow.md](knowledge/05-workflow.md) | 脚本用法、新增工程、提交推送 |
| [06-tuning.md](knowledge/06-tuning.md) | 麦克风灵敏度、音阶、灯的参数 |
| [07-troubleshooting.md](knowledge/07-troubleshooting.md) | 编译 / 烧录 / 外设 / 同步排错 |
| [08-glossary.md](knowledge/08-glossary.md) | I2S、RMT、dBFS 等术语解释 |
| [09-roadmap.md](knowledge/09-roadmap.md) | 后续可做的扩展方向 |
| [10-xiaozhi-assistant.md](knowledge/10-xiaozhi-assistant.md) | AI 随身助手应用：双网络、传感器工具、摇一摇、构建与已知问题 |
| [11-ml307-component.md](knowledge/11-ml307-component.md) | esp-ml307 组件结构、AT 通道与本地副本说明 |

## 快速开始

### 硬件验证示例

```powershell
# 1. 构建（默认 build 目录 D:\esp32\build\<工程名>）
.\tools\build.ps1 -Project 01_blink_led

# 2. 烧录
.\tools\flash.ps1 -Project 01_blink_led -Port COM4

# 3. 串口监视（Ctrl+] 退出）
.\tools\monitor.ps1 -Project 01_blink_led -Port COM4
```

构建产物默认放在短路径 `D:\esp32\build\<工程名>`，以规避 Windows 的 MAX_PATH 限制；如需改动，先设置环境变量 `ESP32_BUILD_ROOT` 再运行脚本。

### AI 随身助手

```powershell
cd xiaozhi-esp32
python scripts/build.py goouuu-s3-dual
idf.py -p COM4 flash monitor
```

`xiaozhi-esp32/build` 是本地构建目录，已随搬迁清理；重新构建会自动生成。历史固件归档在
`xiaozhi-esp32/releases/goouuu-s3-dual_v2.4.2_20261021/`（含整包、分文件和烧录说明）。

## 目录结构

```
esp32s3-goouuu-projects\
├─ README.md
├─ docs\                    板子资料
│   ├─ environment.md
│   ├─ board.md
│   └─ assets\              （原理图、板卡 HTML 参考页）
├─ knowledge\               知识库（上手 / 架构 / 调参 / 排错 / 术语 / 路线）
├─ tools\                   统一构建 / 烧录 / 监视脚本
├─ 01_blink_led\
├─ 02_speaker_tone\
├─ 03_mic_level\
├─ 04_mpu6050_sound\
├─ 05_clap_echo\
├─ 06_offline_pet\
├─ 07_env_sensor\
├─ 08_st7789_lcd\
├─ 09_ml307_probe\
├─ xiaozhi-esp32\           AI 随身助手（独立 Git 仓库）
└─ esp-ml307-src\           4G 组件源码（独立 Git 仓库）
```
