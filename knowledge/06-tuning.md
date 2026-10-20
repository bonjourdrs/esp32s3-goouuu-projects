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

## 离线宠物

全部阈值集中在 `06_offline_pet/main/config/demo_config.h`。

| 常量 | 默认 | 作用 |
|---|---|---|
| `VOICE_START_MULTIPLIER` | 3.0 | 起判音量 = 噪声底 × 该倍数，说话轻就调到 2.0 |
| `VOICE_START_HOLD_MS` | 300 | 音量需持续超过起判值的时长 |
| `VOICE_END_HOLD_MS` | 500 | 判定「说完了」所需的静音时长，也是响应延迟的主要来源 |
| `MOTION_STILL_G` | 0.08 | 重力残差低于此值算静止 |
| `MOTION_MOVE_G` | 0.12 | 残差高于此值算运动 |
| `PICKUP_STILL_HOLD_MS` | 400 | 拿起前需要先静止多久 |
| `PICKUP_MOVE_HOLD_MS` | 150 | 拿起需要持续运动多久 |
| `TAP_THRESHOLD_G` | 2.5 | 敲击的加速度模长门槛 |
| `TAP_MAX_WIDTH_MS` | 40 | 超过这个脉宽就不算敲击（摇晃会超） |
| `TAP_REST_BAND_G` | 0.35 | 敲击必须从 1 g 附近起跳 |
| `SHAKE_WINDOW_MS` | 800 | 摇晃的滑动窗口 |
| `SHAKE_PEAK_G` | 0.6 | 计入摇晃的峰高门槛 |
| `SHAKE_HARD_PEAK_G` | 1.0 | 窗口内最大峰需超过此值才算猛摇 |
| `SHAKE_HARD_PEAK_COUNT` | 3 | 窗口内需要超过这么多个峰 |
| `UPSIDE_DOWN_AZ_G` | −0.7 | az 低于此值算倒扣 |
| `UPSIDE_DOWN_HOLD_MS` | 800 | 倒扣需保持多久 |

调参前先看串口。`motion` 任务每 40 个采样点打一行 `MOTION: mag=.. dyn=..`，
其中 `dyn` 是重力残差，也就是「纯运动强度」，是判断拿起 / 摇晃门槛的直接依据；
`mag` 是含重力的模长，用于敲击。`audio_capture` 打的是麦克风 `rms`。

几条实测结论：

- **采样率实际是 100 Hz。** 代码请求 200 Hz，但默认 RTOS tick 是 100 Hz，
  `pdMS_TO_TICKS(5)` 整除后得 0，`vTaskDelayUntil` 会直接断言崩溃。现在用 1 tick 兜底。
  所有判定都用 `esp_timer` 计时，所以只影响分辨率，不影响阈值语义。
- **敲击与摇晃靠「起跳点」区分。** 只看模长阈值和脉宽不够：摇晃的每个半周期也可能短于 40 ms。
  加一条「前一采样点必须落在 1 g ±0.35 g」之后，摇晃和拿起不再被误判成敲击。
- **拿起事件要求先静止。** 连续动作之间若没有 400 ms 以上的静止，拿起不会触发——
  这是设计如此，不是 bug。
- **IMU 上电首次探测偶发失败。** 传感器和功放共用 3V3，功放使能后首次 I2C 探测更容易失败。
  现在把 IMU 初始化提到扬声器之前，并加了 5 次探测重试。

