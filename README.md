# ESP32-S3 GOOUUU 果云版 · 六工程示例

ESP32-S3（N16R8，16 MB Flash + 8 MB Octal PSRAM）开发板上的六个**独立** ESP-IDF 示例工程，
从单外设验证（点灯、扬声器、麦克风、六轴传感器）逐步走到多模块联动的完整闭环。

## 工程列表

| 目录 | 功能 | 外设 | 引脚 |
|---|---|---|---|
| [`01_blink_led`](01_blink_led) | 板载 RGB 灯循环变色 + 心跳日志 | WS2812（RMT） | GPIO48 |
| [`02_speaker_tone`](02_speaker_tone) | 每 2 秒播放 do-re-mi-sol-do 音阶 | MAX98357（I2S TX） | BCLK=15 / LRC=16 / DIN=7 |
| [`03_mic_level`](03_mic_level) | 麦克风音量 → RGB 灯亮度 | INMP441（I2S RX）+ WS2812 | WS=4 / SCK=5 / SD=6 |
| [`04_mpu6050_sound`](04_mpu6050_sound) | 摇晃越强，音调越高、音量越大，静止淡出 | MPU6050（I2C）+ MAX98357 | SDA=8 / SCL=9；BCLK=15 / LRC=16 / DIN=7 |
| [`05_clap_echo`](05_clap_echo) | 拍手触发回声，倾斜板子换音高，灯随音量闪 | INMP441 + MPU6050 + MAX98357 + WS2812 | WS=4 / SCK=5 / SD=6；SDA=8 / SCL=9；BCLK=15 / LRC=16 / DIN=7 |
| [`06_offline_pet`](06_offline_pet) | 完全离线桌面宠物：说话/拿起/敲击/猛摇/倒扣各有回应，情绪会累积 | INMP441 + MPU6050 + MAX98357 + WS2812 | WS=4 / SCK=5 / SD=6；SDA=8 / SCL=9；BCLK=15 / LRC=16 / DIN=7 |

六个工程各自独立编译、烧录，互不依赖。

## 硬件

- 核心板：ESP32-S3-DevKitC-1 42Pin（果云版），N16R8 模组（16 MB Flash / 8 MB Octal PSRAM）
- 底板：2×21P 母座，板载 WS2812 RGB LED、MAX98357 I2S 功放、INMP441 数字麦克风等
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
| [04-architecture.md](knowledge/04-architecture.md) | 模块划分与音频数据流 |
| [05-workflow.md](knowledge/05-workflow.md) | 脚本用法、新增工程、提交推送 |
| [06-tuning.md](knowledge/06-tuning.md) | 麦克风灵敏度、音阶、灯的参数 |
| [07-troubleshooting.md](knowledge/07-troubleshooting.md) | 编译 / 烧录 / 外设 / 同步排错 |
| [08-glossary.md](knowledge/08-glossary.md) | I2S、RMT、dBFS 等术语解释 |
| [09-roadmap.md](knowledge/09-roadmap.md) | 后续可做的扩展方向 |

## 快速开始

```powershell
# 1. 构建（默认 build 目录 D:\esp32\build\<工程名>）
.\tools\build.ps1 -Project 01_blink_led

# 2. 烧录
.\tools\flash.ps1 -Project 01_blink_led -Port COM4

# 3. 串口监视（Ctrl+] 退出）
.\tools\monitor.ps1 -Project 01_blink_led -Port COM4
```

构建产物默认放在短路径 `D:\esp32\build\<工程名>`，以规避 Windows 的 MAX_PATH 限制；
如需改动，先设置环境变量 `ESP32_BUILD_ROOT` 再运行脚本。

## 目录结构

```
.
├─ README.md
├─ docs\            板子资料
│   ├─ environment.md
│   ├─ board.md
│   └─ assets\      （原理图、板卡 HTML 参考页）
├─ knowledge\       知识库（上手 / 架构 / 调参 / 排错 / 术语 / 路线）
├─ tools\           统一构建 / 烧录 / 监视脚本
├─ 01_blink_led\
├─ 02_speaker_tone\
├─ 03_mic_level\
├─ 04_mpu6050_sound\
├─ 05_clap_echo\
└─ 06_offline_pet\
```
