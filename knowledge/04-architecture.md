# 代码架构

## 仓库分层

仓库根只放跨工程的东西：`README.md`、`.gitignore`、`docs/`、`knowledge/`、`tools/`。每个工程是一个完整独立的 ESP-IDF 工程，有自己的 `CMakeLists.txt`、`sdkconfig.defaults` 和 `main/`，可以单独编译烧录，互不引用。

```
esp32s3-goouuu-projects\
├─ 01_blink_led\        WS2812 循环彩灯
├─ 02_speaker_tone\     MAX98357 音阶播放
├─ 03_mic_level\        麦克风音量 → RGB 亮度
├─ 04_mpu6050_sound\    摇晃强度 → 音高与音量
├─ 05_clap_echo\        拍手 → 回声，倾斜选音高
├─ 06_offline_pet\      离线桌面宠物（说 / 拿 / 敲 / 摇 / 翻）
├─ 07_env_sensor\       温湿度 + 气压（AHT20 + BMP280）
├─ docs\                板子资料（原理图、引脚手册）
├─ knowledge\           本知识库
└─ tools\               构建 / 烧录 / 监视脚本
```

模块源码在用到它的工程里各存一份：`ws2812` 出现在 01、03、05、06，麦克风采集出现在 03、05、06，IMU 出现在 04、05、06。这是刻意的，好处是任何工程单独拷走一个目录就能编译；代价是改一处要同步改几处。等共享代码再重一些，可以抽成公共组件用 `EXTRA_COMPONENT_DIRS` 引入。

## 单个工程的内部结构

前五个工程目录结构一致，以 03 为例：

```
03_mic_level\
├─ CMakeLists.txt        项目入口，声明 project(mic_level)
├─ sdkconfig.defaults    板级默认配置（Flash 16MB、PSRAM、CPU 240MHz）
└─ main\
    ├─ CMakeLists.txt    注册源文件、声明组件依赖
    ├─ Kconfig.projbuild menuconfig 里的 LED 选项（01、03、05 有）
    ├─ main.c            app_main 与任务
    ├─ mic_level.c/.h    INMP441 采集与音量计算
    └─ ws2812.c/.h       WS2812 驱动
```

`sdkconfig` 是构建时生成的文件，不入库。板级差异（Flash 容量、PSRAM 模式、分区表）统一写在 `sdkconfig.defaults` 里，所以各工程这一份内容完全相同。

`06_offline_pet` 是唯一例外：它的文件多得多，所以 `main/` 按职责分了子目录——`config/`（引脚与阈值）、`drivers/`、`audio/`、`motion/`、`events/`、`app/`、`assets/`。`main/CMakeLists.txt` 用相对路径列出各子目录里的源文件，并把每个子目录加进 `INCLUDE_DIRS`，头文件才能互相包含。规模再大时，可以按子目录拆成独立 component。

`main/CMakeLists.txt` 的组件依赖决定能不能编过。ESP-IDF 5.x 把 I2S、RMT、I2C 这些驱动拆成了独立组件，漏掉对应项会在编译期报找不到头文件：

| 工程 | REQUIRES | PRIV_REQUIRES |
|---|---|---|
| 01_blink_led | driver | esp_driver_rmt, esp_timer |
| 02_speaker_tone | driver | esp_driver_i2s |
| 03_mic_level | driver | esp_driver_rmt, esp_driver_i2s |
| 04_mpu6050_sound | driver | esp_driver_i2c, esp_driver_i2s, esp_timer |
| 05_clap_echo | driver | esp_driver_rmt, esp_driver_i2s, esp_driver_i2c, esp_timer |
| 06_offline_pet | driver | esp_driver_rmt, esp_driver_i2s, esp_driver_i2c, esp_timer |
| 07_env_sensor | driver | esp_driver_i2c, esp_driver_rmt |

## 模块职责

ws2812 是对 RMT 的薄封装，对外只有初始化和设置 RGB 两个函数。内部把 24 位颜色按 WS2812 时序编码后交给 RMT 发送：RMT 分辨率设成 10 MHz，一个计数单位 0.1 µs，位的电平持续时间按手册填（0 码 0.3 µs 高，1 码 0.9 µs 高）。数据按 GRB 顺序发，这是 WS2812 的规定，写代码时容易按 RGB 想当然。

mic_level 用 I2S 标准模式做接收端，采样率 16 kHz，每个测量窗读 256 帧，约 16 ms。INMP441 输出 24 位数据、在 32 位槽里左对齐，所以要把样本右移 8 位还原成 24 位有符号数再参与运算。

audio_tone（02）和 audio_out（05）都用 I2S 标准模式做发送端，区别在播放方式。audio_tone 是阻塞式的：给定频率和时长，按 44100 Hz 逐帧算 `sin(phase)`，算完整段再一次性写完。audio_out 是流式的：每次只生成 10 ms 的一块，写完返回，由任务循环反复调用，空闲块输出静音——这样别的任务可以随时插进来触发一个音，而不必等当前音放完。

imu 走 I2C，接在 GPIO8/GPIO9，先探测 0x68 和 0x69 两个地址再读 `WHO_AM_I` 确认型号。这块板实测返回 0x70，是 MPU6500 而不是 MPU6050，两者寄存器兼容，但 MPU6500 的加速度低通滤波器在单独的 0x1d 寄存器里，初始化时要区别对待。倾角来自低通滤波后的重力向量，滤波是为了晃动时读数不乱跳。

## 麦克风到灯的数据流

03_mic_level 的链路值得看一遍：

```
I2S 采 256 帧（16 kHz）
   ↓  样本 >> 8，还原 24 位有符号数
   ↓  平方求和再开方 → RMS
   ↓  20·log10(RMS / 2^23) → dBFS
   ↓  在 [-55, -5] dB 区间线性归一化 → 0..1
   ↓  一阶平滑 0.6·上次 + 0.4·本次
   ↓  平方（gamma 曲线，压低小音量）
   ↓  ×255 → 亮度
WS2812 白灯
```

归一化和 gamma 作用不同。归一化把物理量映射到 0..1，决定「多响算满」；gamma 决定「同样一段变化，人眼看到多少」。少了 gamma，很轻的声音也会把灯推到中亮度，安静和说话区分不出来。

`mic_level_read()` 从 I2S 的 DMA 缓冲里取 256 帧，约 16 ms 的数据量。任务在两次读取之间 `vTaskDelay(30ms)`，这段时间麦克风已经往 DMA 缓冲里攒了更多数据，所以读取几乎不阻塞，循环周期由 `vTaskDelay` 决定，实测约 30 ms。日志每 25 次循环打印一行，串口上正好 750 ms 一条，与这个周期吻合。

## 拍手回声的数据流

05_clap_echo 是三个模块联动的例子，三个任务通过一个队列和一个共享变量连起来：

```
mic_level_read()  ──►  音量 level
                        │
        ┌───────────────┴────────────────┐
        │                                │
   跳变判定                          环境音量
   level > 0.65                      平滑 + gamma
   level - prev > 0.20                 ↓
   距上次 > 180 ms                 灯的基础亮度
        │                                │
     判定为拍手 ──────────────────────► 取较大值 ──► WS2812
        │
   读 s_note_index（倾斜决定）          imu_tilt_deg() ──► 倾角 ──► 音档
        │                                                      ▲
   写入 clap 队列 ──► audio_task ──► audio_out_trigger(频率)     │
                          │                                 imu_task（20 ms）
                     audio_out_pump()
                     每 10 ms 一块
                          │
                    I2S TX ──► MAX98357
```

拍手判定看的是跳变而不是绝对值：只看绝对音量的话，说话声和持续噪声都会误触发。三个条件缺一不可——当前窗口够响、比上一窗口明显更高、距上次拍手够久。

音频任务和麦克风任务分属两个 I2S 控制器，因为麦克风和功放的引脚是两组独立的时钟线，同一个控制器做不到同时收发。ESP32-S3 正好有两个 I2S 外设，各占一个。音频任务优先级设得比麦克风任务高，避免它被抢占导致 DMA 断流。

## 启动顺序

各工程的 `app_main` 都很短：打印一行固件构建时间与引脚信息，初始化外设，创建任务，然后返回。实际工作全在任务里。这样做的好处是 `app_main` 返回后 FreeRTOS 调度器接管，任务按优先级和 `vTaskDelay` 自然让出 CPU，不需要自己写忙等循环。

外设初始化失败的处理方式不同：I2S 和 RMT 是必需通路，用 `ESP_ERROR_CHECK` 直接中止；IMU 是可选输入，失败了只打印警告并跳过对应任务，程序照常运行，只是所有拍手都回同一个音。

## 离线宠物的数据流

`06_offline_pet` 把前面几个模块串成一个闭环，也是唯一严格按「driver → detector → 事件 → 决策 → 播放」分层的工程：

```
麦克风 ──► mic_inmp441 ──► audio_detect  ─┐
                                          │
MPU6050 ─► imu_mpu6050 ──► motion_detect ─┤
                                          ▼
                                    app_event（队列）
                                          ▼
                                       pet_app
                                          ▼
                                   audio_player ──► speaker_max98357
```

分层是硬约束：driver 只把寄存器读成物理量，detector 只判断「发生了什么」，pet_app 只决定「怎么回应」，player 只负责把 PCM 送出去。detector 不碰扬声器，pet_app 不碰 I2C/I2S，传感器之间不互相调用——全部经过队列。这样加一个新动作，只需要写一个 detector 加一条事件分支，不用改其它任何一层。

**audio_detect 的阈值是自适应的。** 它维护一个噪声底估计，只在判定为安静时用 `noise += 0.02 × (rms − noise)` 更新。开始说话的门槛是噪声底的 3 倍、结束是 1.8 倍，所以风扇声、电视背景音不会让检测失效，也不用为每个房间重调一个固定 dBFS 常量（03/05 用的就是固定常量，换个环境就得改）。判定「说完」需要静音持续 500 ms，这个保持时间就是回应延迟的主要来源。

**motion_detect 用的是重力残差。** 加速度计分不清重力和运动，所以 detector 自己维护一个慢速重力估计（一阶低通，时间常数约 0.25 s），把每个轴的估计减掉，剩下的就是纯粹的动态加速度。静止时残差接近 0，晃动时才会抬起来。这比 `|a| − 1g` 稳，因为后者在板子倾斜时会被重力分量污染。

**敲击和摇晃靠脉宽区分。** 两者都会让 `|a|` 冲过阈值，区别在持续时间：敲击是 1~3 个采样点（5~15 ms）的尖峰，摇晃则维持几十毫秒。所以 TAP 除了阈值还要求脉宽 ≤ 40 ms，超时就把这次越界标记为「不是敲击」。摇晃则用 500 ms 滑动窗口统计动态加速度峰值个数，超过 6 个且峰值 >1.5 g 才算猛摇，配 700 ms 冷却保证「一次摇晃只回一句话」。

**扬声器自反馈的处理。** 麦克风和功放在同一块板上，宠物说话时麦克风会听见它自己，如果不管就会形成「听见自己 → 判定说完 → 再说话」的正反馈。做法是 `audio_player_is_active()`：播放期间以及结束后 300 ms 都返回 true，`audio_detect` 一旦看到它就把状态机复位回静音态并直接返回，不产生任何事件。代价是宠物说话时听不见用户，这是刻意的取舍。

**音频素材直接编进 flash。** 5 段 WAV 放在 `main/assets/wav/`，由 `main/CMakeLists.txt` 的 `EMBED_FILES` 交给链接器，符号名按路径生成（`_binary_assets_wav_hello_wav_start`）。`audio_assets.c` 在运行时遍历 RIFF 块找到 `data`，而不是假设固定的 44 字节头——这样换编码器加了额外块也不会崩。不需要文件系统，不需要 SD 卡，烧录即运行。
