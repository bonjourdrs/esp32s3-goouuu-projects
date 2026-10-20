# 06_offline_pet · 完全离线的桌面宠物

一块会「有反应」的板子。它能听见你说话并回应，知道你把它拿起来、敲它、猛摇它、把它翻过来，
并且对每种动作的情绪反应会累积——被敲多了会生气，被摇多了会晕。全部逻辑跑在板上，
**不联网、不用云端 ASR/LLM/TTS、不做语音识别**。

## 交互

| 动作 | 检测方式 | 回应 | 情绪变化 |
|---|---|---|---|
| 说完一句话 | 音量超过噪声底 3 倍并保持 300 ms，之后静音 500 ms | `hello.wav` — "I hear you!" | mood +2 |
| 把板子拿起来 | 静止 ≥400 ms 后持续运动 ≥150 ms | `pickup.wav` — "Hey! You picked me up." | mood +3 |
| 用手指敲一下 | 从静止起跳、加速度模长 >2.5 g 且脉宽 ≤40 ms | `ouch.wav` — "Ouch!" | anger +10 |
| 猛摇 | 800 ms 窗口内 >3 个 >0.6 g 的动态加速度峰，且峰值 >1.0 g | `shake_hard.wav` — "Stop it, I'm dizzy!" | dizzy +30，anger +5 |
| 倒扣板子 | az < −0.7 g 持续 800 ms | `upside_down.wav` — "Whoa, I'm upside down!" | — |

板载 WS2812 是情绪灯：**红 = anger，绿 = mood，蓝 = dizzy**，三种按比例混色；每次回应叠一层白光脉冲。

## 接线

全部用板载外设，与 02/03/04/05 完全相同：

| 外设 | 引脚 |
|---|---|
| INMP441 麦克风 | WS=4 / SCK=5 / SD=6 |
| MAX98357 功放 | BCLK=15 / LRC=16 / DIN=7 |
| MPU6050/6500 | SDA=8 / SCL=9 |
| WS2812 RGB | GPIO48 |

引脚只在 `main/config/board_config.h` 里定义，业务代码不出现裸引脚号。

## 架构

严格分层，任何一层都不越界：

```
                  ┌──────────────┐
   麦克风 ──► mic_inmp441        │
                  └──────┬───────┘
                         ▼
                  audio_detect ──►┐
                                  │
                  ┌──────────────┐│
   MPU6050 ──► imu_mpu6050        ││
                  └──────┬───────┘│
                         ▼        │
                  motion_detect ──┤
                                  ▼
                          ┌──────────────┐
                          │  app_event   │  统一事件队列
                          └──────┬───────┘
                                 ▼
                          ┌──────────────┐
                          │   pet_app    │  情绪 + 决策
                          └──────┬───────┘
                                 ▼
                          ┌──────────────┐
                          │ audio_player │
                          └──────┬───────┘
                                 ▼
                          speaker_max98357 ──► 功放
```

规则：**driver 只碰硬件，detector 只判断「发生了什么」，pet_app 只决定「怎么回应」，
player 只负责播放**。detector 不直接放音，pet_app 不直接读 I2C/I2S，所有传感器之间通过事件解耦。

## 任务

| 任务 | 优先级 | 周期 | 职责 |
|---|---|---|---|
| `audio_capture` | 6 | 20 ms（I2S 帧长） | 采一帧 → RMS → `audio_detect` → 发事件 |
| `audio_player` | 6 | 事件驱动 | 从 flash 流式播放，维护自反馈 guard 窗口 |
| `motion` | 5 | 10 ms（受 RTOS tick 限制，约 100 Hz） | 读 IMU → `motion_detect` → 发事件 |
| `pet` | 4 | 50 ms 唤醒 | 消费事件、更新情绪、驱动灯 |

## 两个关键设计

**扬声器自反馈。** 麦克风和功放在同一块板上，宠物说话时麦克风会听见它自己。
解法是 `audio_player_is_active()`：播放期间以及结束后 300 ms，`audio_detect` 不产生任何事件，
并把自己复位回静音态。代价是宠物说话时听不见你——这是刻意取舍，不是 bug。

**敲击 vs 摇晃。** 两者都会让加速度模长冲过阈值，区别在持续时间：敲击是 1~3 个采样点
（5~15 ms）的尖峰，摇晃则要维持几十毫秒。所以 TAP 判定除了阈值，还要求脉宽 ≤ `TAP_MAX_WIDTH_MS`。
但仅靠脉宽还不够——摇晃的每个半周期也可能短于 40 ms。所以再加一条：敲击必须**从静止起跳**，
即越过阈值的前一个采样点要落在 1 g 附近（`TAP_REST_BAND_G`）；摇晃时板子一直在动，这一条会把
它挡掉。

## 参数

所有阈值集中在 `main/config/demo_config.h`，没有一个 magic number 散落在 detector 里。
改动后重新编译即可，详见 [knowledge/06-tuning.md](../knowledge/06-tuning.md) 的「离线宠物」一节。

## 音频素材

5 段语音以 WAV 形式放在 `main/assets/wav/`，通过 `main/CMakeLists.txt` 的 `EMBED_FILES`
编译进 flash——不需要文件系统、不需要 SD 卡、烧录即可运行。总大小约 162 KB。

素材由本机 Windows TTS 生成（英文女声 Zira），脚本在 `tools/make_pet_audio.ps1`：

```powershell
.\tools\make_pet_audio.ps1              # 重新生成全部 5 段
.\tools\make_pet_audio.ps1 -Rate 3      # 语速更快
```

脚本按固件要求的格式（16 kHz / 16-bit / mono）合成，并裁掉首尾静音。
`assets/audio_assets.c` 会在运行时解析 RIFF 块找到 `data`，而不是假设固定的 44 字节头。

> 本机没有安装中文语音，所以台词是英文。想换中文，需要先在
> 「Windows 设置 → 时间和语言 → 语音」里添加中文语音包，再改脚本里的 `-Voice` 参数。

## 已知限制

- **回应延迟 ≥500 ms**：VOICE_END 必须等静音维持 500 ms 才能判定说完，这是机制决定的。
- **敲击阈值依赖外壳**：裸板与装入外壳后的敲击波形差别很大，装壳后必须重调 `TAP_THRESHOLD_G`。
- **拿起过程可能误报**：拿起时必然伴随倾斜和加速度，PICK_UP 与 SHAKE/TAP 之间靠脉宽、持续时间和
  冷却时间区分，参数需要在真实使用中收敛。
- **第一版不含**：CLAP、PUT_DOWN、轻/中摇、倾斜、双敲，以及 AEC / ASR / 关键词唤醒。

## 快速开始

```powershell
.\tools\build.ps1  -Project 06_offline_pet
.\tools\flash.ps1  -Project 06_offline_pet -Port COM4
.\tools\monitor.ps1 -Project 06_offline_pet -Port COM4
```
