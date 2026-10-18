# ESP32-S3 GOOUUU 果云版 · 三工程示例

ESP32-S3（N16R8，16 MB Flash + 8 MB Octal PSRAM）开发板上的三个**独立** ESP-IDF 示例工程，
分别验证点灯、扬声器、麦克风三条通路。

## 工程列表

| 目录 | 功能 | 外设 | 引脚 |
|---|---|---|---|
| [`01_blink_led`](01_blink_led) | 板载 RGB 灯循环变色 + 心跳日志 | WS2812（RMT） | GPIO48 |
| [`02_speaker_tone`](02_speaker_tone) | 每 2 秒播放 do-re-mi-sol-do 音阶 | MAX98357（I2S TX） | BCLK=15 / LRC=16 / DIN=7 |
| [`03_mic_level`](03_mic_level) | 麦克风音量 → RGB 灯亮度 | INMP441（I2S RX）+ WS2812 | WS=4 / SCK=5 / SD=6 |

三个工程各自独立编译、烧录，互不依赖。

## 硬件

- 核心板：ESP32-S3-DevKitC-1 42Pin（果云版），N16R8 模组（16 MB Flash / 8 MB Octal PSRAM）
- 底板：2×21P 母座，板载 WS2812 RGB LED、MAX98357 I2S 功放、INMP441 数字麦克风等
- 详细引脚与模块说明见 [docs/board.md](docs/board.md)

## 开发环境

- ESP-IDF **v5.5.6**（`D:\Espressif\esp-idf`）
- 工具链 xtensa-esp32s3-elf，Python 3.10.11
- 完整环境说明见 [docs/environment.md](docs/environment.md)

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
├─ docs\            环境与板卡资料
│   ├─ environment.md
│   ├─ board.md
│   └─ assets\      （原理图、板卡 HTML 参考页）
├─ tools\           统一构建 / 烧录 / 监视脚本
├─ 01_blink_led\
├─ 02_speaker_tone\
└─ 03_mic_level\
```
