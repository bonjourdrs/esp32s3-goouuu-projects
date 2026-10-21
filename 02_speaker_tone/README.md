# 扬声器音阶测试

单独验证 MAX98357 I2S 功放和喇叭通路：上电后每 2 秒播放一遍 do-re-mi-sol-do，听到声音就说明 I2S 输出、功放使能和接线都正常。

## 行为

- 采样率 44.1 kHz、16 bit、立体声（单声道样本复制到左右声道），幅度 9000（约满量程 27%）。
- 音符序列固定为 523 / 587 / 659 / 784 / 1047 Hz，时长 260 / 260 / 260 / 320 / 520 ms，音符之间静音 70 ms。
- 每轮开始打印 `playing test scale`；I2S 初始化失败会打印错误并停止任务，不会反复重启。

## 接线

| MAX98357 | ESP32-S3 | 说明 |
|---|---|---|
| BCLK | GPIO15 | 位时钟 |
| LRC | GPIO16 | 左右声道时钟 |
| DIN | GPIO7 | 数据输入 |
| SD | 3V3 | 常开，不受 GPIO 控制 |
| GAIN | GND | 默认增益 |
| VIN / GND | 5V / GND | 功放供电，按底板实际走线 |

不使用 MCLK，`audio_tone.c` 里 `mclk = I2S_GPIO_UNUSED`。

## 运行

```powershell
.\tools\build.ps1   -Project 02_speaker_tone
.\tools\flash.ps1   -Project 02_speaker_tone -Port COM4
.\tools\monitor.ps1 -Project 02_speaker_tone -Port COM4
```

## 验证现象

串口每 2 秒出现一行：

```
I (2000) SPEAKER: playing test scale
```

同时喇叭播完五个音。没有声音时按顺序检查：SD 是否接 3V3、GAIN 是否接 GND、BCLK/LRC/DIN 是否接反、喇叭是否接在功放输出端。

## 文件结构

| 文件 | 作用 |
|---|---|
| `main/main.c` | 创建播放任务，每 2 秒触发一次音阶 |
| `main/audio_tone.c/.h` | I2S 初始化、正弦波生成、阻塞式音符播放 |

## 调参

- 音高和时长：`audio_tone.c` 的 `audio_tone_play_scale()` 里两个数组一一对应，加长数组就能加音符。
- 音量和采样率：同文件顶部的 `AMPLITUDE`（默认 9000）和 `SAMPLE_RATE`（默认 44100）。
- 缓冲区：`TONE_BUF_FRAME` 默认 256 帧，增大可减少写入次数，但会增加延迟。
