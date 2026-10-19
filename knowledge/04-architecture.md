# 代码架构

## 仓库分层

仓库根只放跨工程的东西：`README.md`、`.gitignore`、`docs/`、`knowledge/`、`tools/`。每个工程是一个完整独立的 ESP-IDF 工程，有自己的 `CMakeLists.txt`、`sdkconfig.defaults` 和 `main/`，可以单独编译烧录，互不引用。

```
esp32s3-goouuu-projects\
├─ 01_blink_led\        WS2812 循环彩灯
├─ 02_speaker_tone\     MAX98357 音阶播放
├─ 03_mic_level\        麦克风音量 → RGB 亮度
├─ docs\                板子资料（原理图、引脚手册）
├─ knowledge\           本知识库
└─ tools\               构建 / 烧录 / 监视脚本
```

ws2812 驱动在 01 和 03 里各存一份，是刻意的：这样两个工程谁都不依赖谁，单独拷走一个目录就能编译。代价是改一处要改两处。等工程数量变多、共享代码变重，再考虑抽成公共组件用 `EXTRA_COMPONENT_DIRS` 引入。

## 单个工程的内部结构

三个工程目录结构一致，以 03 为例：

```
03_mic_level\
├─ CMakeLists.txt        项目入口，声明 project(mic_level)
├─ sdkconfig.defaults    板级默认配置（Flash 16MB、PSRAM、CPU 240MHz）
└─ main\
    ├─ CMakeLists.txt    注册源文件、声明组件依赖
    ├─ Kconfig.projbuild menuconfig 里的 LED 选项（01、03 有）
    ├─ main.c            app_main 与任务
    ├─ mic_level.c/.h    INMP441 采集与音量计算
    └─ ws2812.c/.h       WS2812 驱动
```

`sdkconfig` 是构建时生成的文件，不入库。板级差异（Flash 容量、PSRAM 模式、分区表）统一写在 `sdkconfig.defaults` 里，所以三个工程这一份内容完全相同。

`main/CMakeLists.txt` 的组件依赖决定能不能编过。ESP-IDF 5.x 把 I2S、RMT 这些驱动拆成了独立组件，漏掉对应项会在编译期报找不到头文件：

| 工程 | REQUIRES | PRIV_REQUIRES |
|---|---|---|
| 01_blink_led | driver | esp_driver_rmt, esp_timer |
| 02_speaker_tone | driver | esp_driver_i2s |
| 03_mic_level | driver | esp_driver_rmt, esp_driver_i2s |

## 三个模块的职责

ws2812 是对 RMT 的薄封装，对外只有初始化和设置 RGB 两个函数。内部把 24 位颜色按 WS2812 时序编码后交给 RMT 发送：RMT 分辨率设成 10 MHz，一个计数单位 0.1 µs，位的电平持续时间按手册填（0 码 0.3 µs 高，1 码 0.9 µs 高）。数据按 GRB 顺序发，这是 WS2812 的规定，写代码时容易按 RGB 想当然。

audio_tone 用 I2S 标准模式做发送端。它不播采样文件，而是实时算正弦波：给定频率和时长，按 44100 Hz 逐帧算 `sin(phase)`，乘幅度后写成 16 位立体声样本发给功放。音阶就是依次调用五次不同频率。

mic_level 用 I2S 标准模式做接收端，采样率 16 kHz，每个测量窗读 256 帧，约 16 ms。INMP441 输出 24 位数据、在 32 位槽里左对齐，所以要把样本右移 8 位还原成 24 位有符号数再参与运算。

## 麦克风到灯的数据流

03_mic_level 是这套工程的落点，整条链路值得看一遍：

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

`mic_level_read()` 内部是阻塞的，等满一个 16 ms 窗口才返回，所以调用它的任务循环不会快于 16 ms。当前任务读完再 `vTaskDelay(30ms)`，约 46 ms 更新一次灯，视觉上跟得上声音。

## 启动顺序

三个工程的 `app_main` 都很短：打印一行固件构建时间与引脚信息，初始化外设，创建任务，然后返回。实际工作全在任务里。这样做的好处是 `app_main` 返回后 FreeRTOS 调度器接管，任务按优先级和 `vTaskDelay` 自然让出 CPU，不需要自己写忙等循环。
