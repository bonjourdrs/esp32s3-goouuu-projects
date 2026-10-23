# 快速上手

## 需要准备什么

一块 GOOUUU 果云版 ESP32-S3 底板（插好 42Pin 核心板）、一台装了 ESP-IDF v5.5.6 的 Windows 电脑、一根能传数据的 USB-C 线。线材值得留意：市面上大量 USB-C 线只供电不传数据，插上后电脑看不到串口，多半是线的问题而不是板子的问题。

工具链还没装的，先看 [02-toolchain-setup.md](02-toolchain-setup.md)。

## 三步跑起来

```powershell
cd D:\esp32\esp32s3-goouuu-projects
.\tools\build.ps1   -Project 01_blink_led
.\tools\flash.ps1   -Project 01_blink_led -Port COM4
.\tools\monitor.ps1 -Project 01_blink_led -Port COM4
```

三条命令依次完成：编译出固件、写进 Flash、打开串口看日志。`-Port` 换成设备管理器里实际的端口号，不同机器、不同 USB 口都可能变。

## 应该看到什么

01_blink_led 烧进去后，板上那颗 RGB 灯每 500 ms 在红绿蓝黄青紫白之间循环一次；串口每 5 秒打一行心跳，带运行秒数和剩余堆内存：

```
I (12345) BLINK: heartbeat  uptime=5s  free_heap=380000 B  led_step=10
```

出现这行，说明工具链、USB 串口、RMT 驱动、灯这条链路都通了。

另外两个工程验证的是不同通路。02_speaker_tone 每 2 秒播一遍 do-re-mi-sol-do，听到声音就说明 I2S 输出和功放正常。03_mic_level 是这套工程的终点：对着麦克风说话，灯会跟着音量变亮，安静时几乎全灭。

## 换工程

十个示例工程（01–10）互相独立，切换只改 `-Project` 后面的名字。它们不共用构建目录，`D:\esp32\build\` 下会各建一个。

```powershell
.\tools\build.ps1 -Project 03_mic_level
.\tools\flash.ps1 -Project 03_mic_level -Port COM4
.\tools\monitor.ps1 -Project 03_mic_level -Port COM4
```

## 退出串口监视

按 `Ctrl+]`。直接关窗口会留下占用串口的进程，下次烧录会报端口被占用。真遇到这种情况，关掉残留的 `python.exe` 进程，或者拔插一次 USB。
