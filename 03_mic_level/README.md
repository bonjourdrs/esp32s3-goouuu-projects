# 麦克风音量点灯

把 INMP441 采集到的声音大小映射成板载 RGB 灯的亮度：安静时几乎全灭，说话或拍手时明显变亮。用来单独验证麦克风 I2S 接收通路。

## 行为

- 采样率 16 kHz，每 256 帧（约 16 ms）算一次 RMS，再转成 dBFS。
- dBFS 在 -55 dB（静音底噪）到 -5 dB（大声）之间线性归一化到 0.0–1.0，超出范围会截断。
- 亮度做两级处理：先按 `0.6 * 上次 + 0.4 * 本次` 平滑，避免闪烁；再平方（gamma）让低音量更暗、高音量更亮。
- WS2812 输出白光，三通道值相同；普通 LED 模式下电平在亮度超过 0.05 时点亮。
- 每 25 次测量（约 0.4 秒）打印一行 `mic level` 和 `brightness`。

## 接线

| INMP441 | ESP32-S3 | 说明 |
|---|---|---|
| WS | GPIO4 | 字选择 / 左右声道时钟 |
| SCK | GPIO5 | 位时钟 |
| SD | GPIO6 | 数据输出 |
| L/R | GND | 接地选择左声道 |
| VDD / GND | 3V3 / GND | 供电 |

麦克风与功放使用两组独立的 I2S 引脚（功放是 GPIO15/16/7），互不共用时钟。

## 运行

```powershell
.\tools\build.ps1   -Project 03_mic_level
.\tools\flash.ps1   -Project 03_mic_level -Port COM4
.\tools\monitor.ps1 -Project 03_mic_level -Port COM4
```

## 验证现象

```
I (1200) MICLED: mic level=0.02  brightness=0
I (1520) MICLED: mic level=0.63  brightness=102
```

安静时 `level` 在 0.0–0.05 附近，对着麦克风正常说话应能到 0.4–0.8，灯随之变亮。

## 文件结构

| 文件 | 作用 |
|---|---|
| `main/main.c` | 测量循环、平滑与 gamma、灯亮度映射 |
| `main/mic_level.c/.h` | I2S 接收初始化、RMS 与 dBFS 归一化 |
| `main/ws2812.c/.h` | 灯珠驱动 |
| `main/Kconfig.projbuild` | LED 类型、GPIO、闪烁周期 |

## 调参

`main/mic_level.c` 顶部：

| 常量 | 默认 | 作用 |
|---|---|---|
| `MIC_SAMPLE_RATE` | 16000 | 采样率，改动后窗口时长随之变化 |
| `MIC_WINDOW_FRAME` | 256 | 每次测量的帧数，越大越平稳但响应越慢 |
| `MIC_FLOOR_DB` | -55 dB | 归一化下限，环境吵就往上调 |
| `MIC_CEIL_DB` | -5 dB | 归一化上限，麦克风离得远可往下调 |

平滑系数和 gamma 曲线在 `main/main.c` 的测量循环里。

## 注意

INMP441 输出 24 位数据、在 32 位槽里左对齐，代码里先右移 8 位还原成 24 位有符号数再参与运算。如果 `level` 长期为 0，优先检查 L/R 是否接地、SD 是否接到 GPIO6。
