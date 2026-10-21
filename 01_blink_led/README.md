# 板载 RGB 灯循环

最小可运行工程：让底板上的 WS2812 灯珠按固定节奏循环变色，同时在串口打心跳。用来确认工具链、烧录链路和板子供电都正常。

## 行为

- 每 `LED_BLINK_PERIOD_MS`（默认 500 ms）切换一种颜色，顺序为红 → 绿 → 蓝 → 黄 → 青 → 紫 → 白 → 灭。
- 每 10 步（约 5 秒）打印一行心跳，包含运行秒数、剩余堆内存和当前步数：

```
I (5000) BLINK: heartbeat  uptime=5s  free_heap=361234 B  led_step=10
```

- 颜色亮度上限是 32（满量程 255），避免直视刺眼；需要更亮改 `main/main.c` 的颜色表即可。

## 接线

| 器件 | 引脚 | 说明 |
|---|---|---|
| 板载 WS2812 | GPIO48 | 底板丝印 `G48`，单灯，数据线直连 |
| 普通 LED（可选） | 可配 | menuconfig 里改成 `Plain GPIO LED` 并指定 GPIO |

WS2812 的数据协议由 `ws2812.c` 用 RMT 实现：RMT 分辨率 10 MHz（0.1 µs/计数），按 0.3 µs / 0.9 µs 的高低电平区分 0 码和 1 码，颜色按 **GRB** 顺序发送。

## 运行

```powershell
.\tools\build.ps1   -Project 01_blink_led
.\tools\flash.ps1   -Project 01_blink_led -Port COM4
.\tools\monitor.ps1 -Project 01_blink_led -Port COM4
```

## 文件结构

| 文件 | 作用 |
|---|---|
| `main/main.c` | 颜色表、循环任务、心跳日志 |
| `main/ws2812.c/.h` | RMT 编码与发送封装，对外只有 `ws2812_init()` / `ws2812_set_rgb()` |
| `main/Kconfig.projbuild` | LED 类型、GPIO、周期配置 |

## 调参

`idf.py menuconfig` → `ESP32-S3 Blink Demo`：

| 选项 | 默认 | 作用 |
|---|---|---|
| `On-board LED type` | WS2812 | 可切成普通 GPIO LED |
| `LED GPIO number` | 48 | WS2812 一般是 48，老版 DevKitC-1 可能是 38 |
| `Blink period in milliseconds` | 500 | 每步间隔，范围 50–10000 |

## 注意

如果灯不亮，先确认烧录的是这个工程、GPIO 配置是 48；WS2812 对时序敏感，改 `ws2812.c` 里的 RMT 参数后要重新核对 0/1 码的高电平时间。
