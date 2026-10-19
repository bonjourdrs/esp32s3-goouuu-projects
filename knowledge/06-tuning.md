# 参数调优

可调参数都集中在各模块文件顶部，改完重新编译烧录即可。

## 麦克风灵敏度

灯对声音的响应范围由 `03_mic_level/main/mic_level.c` 里这两个常量决定：

```c
#define MIC_FLOOR_DB (-55.0f)   /* 映射到 0，灯灭 */
#define MIC_CEIL_DB  (-5.0f)    /* 映射到 1，灯最亮 */
```

调法：打开 `monitor.ps1`，安静时看日志里的 `level`。如果安静时读数还在 0.4 以上，说明环境噪声底高于 `MIC_FLOOR_DB`，把它往大调（比如 -50），灯才会真正灭。反过来，正常说话时 `level` 到不了 1，就把 `MIC_CEIL_DB` 调小。当前这组值是按实测噪声底约 -47 dBFS 定的。

其余可调项：

| 常量 | 默认 | 影响 |
|---|---|---|
| `MIC_SAMPLE_RATE` | 16000 | 采样率，影响窗口时长和 CPU 占用 |
| `MIC_WINDOW_FRAME` | 256 | 每次读的帧数，越大读数越稳，灯响应越迟钝 |
| `MIC_SAMPLE_SHIFT` | 8 | 24 位数据在 32 位槽里的对齐偏移，填错会得到错误的音量 |
| `MIC_FULL_SCALE` | 8388608 | 2^23，dBFS 的参考满量程 |

## 灯的响应手感

平滑和 gamma 在 `03_mic_level/main/main.c` 的任务循环里：

```c
smoothed = 0.6f * smoothed + 0.4f * level;   /* 平滑 */
float shaped = smoothed * smoothed;          /* gamma */
```

平滑系数里前一项越大，灯越稳也越迟钝；后一项越大，跟得越紧也越闪。gamma 换成 `sqrtf(smoothed)` 方向相反，会让小音量更亮，适合「有点动静就想看到灯亮」的场合。

## 音阶与音量

音阶表在 `02_speaker_tone/main/audio_tone.c` 的 `audio_tone_play_scale()` 里，频率和时长两个数组一一对应：

```c
static const uint16_t notes[] = { 523, 587, 659, 784, 1047 };
static const uint16_t dur_ms[] = { 260, 260, 260, 320, 520 };
```

改频率就是改音高，改时长就是改节奏，想加低音把 523 换成 262 即可。

数字音量由 `AMPLITUDE` 控制，默认 9000，约占满量程 27%。板载 MAX98357 的增益由硬件 `GAIN` 引脚决定（当前接 GND，对应 9 dB），软件这边只调数字幅度。想更响就加大 `AMPLITUDE`，但接近 32767 会开始削波失真，听感上是破音而不是更响。

## WS2812 时序

`01`、`03`、`05` 的 `ws2812.c` 顶部：

```c
#define WS2812_RESOLUTION_HZ 10000000   /* RMT 时钟 10 MHz，1 tick = 0.1 µs */
#define WS2812_T0H 3
#define WS2812_T0L 9
#define WS2812_T1H 9
#define WS2812_T1L 3
```

这四组数值是 WS2812 的位时序，以 0.1 µs 为单位。除非换用别的灯珠（SK6812 或新批次 WS2812B 的时序略有差异），否则不需要动。填错会表现为颜色乱跳或整条灯不亮。

灯的颜色和亮度在 `01_blink_led/main/main.c` 的颜色表里，每项是 RGB 三通道，当前最大值 32 是为了不刺眼；`03` 里亮度由麦克风音量算出来，最大 255。

## 拍手回声

触发灵敏度和音档在 `05_clap_echo/main/main.c` 顶部：

| 常量 | 默认 | 作用 |
|---|---|---|
| `CLAP_LEVEL` | 0.65 | 判定为拍手的最低音量，环境吵就调高 |
| `CLAP_RISE` | 0.20 | 相对上一窗口要求的最小跳变，调小更容易触发 |
| `CLAP_REFRACTORY_MS` | 180 | 两次拍手之间的最短间隔 |
| `s_notes[]` | 523–880 | 五声音阶五个音，加长数组就能加档 |
| `TILT_RANGE_DEG` | 40 | 倾多少度走完全部音档，调小则轻微倾斜就换音 |
| `FLASH_DECAY` | 0.90 | 拍手闪光的每帧衰减系数，越小灭得越快 |

音符长度和数字音量在 `05_clap_echo/main/audio_out.c`：`TONE_FRAMES` 是时长，默认 250 ms；`AMPLITUDE` 是幅度。麦克风灵敏度仍在 `mic_level.c`，和 03 相同。

调触发阈值前先看串口里安静时的 `level`。这个房间的底噪在 0.15~0.45 之间，所以 `CLAP_LEVEL` 不宜低于 0.5，否则说话就会误触发。
