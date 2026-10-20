# 硬件

完整引脚手册在 [docs/board.md](../docs/board.md)，原理图在 [docs/assets/schematic.png](../docs/assets/schematic.png)。这里只留开发时最常需要对照的部分。

## 板子构成

底板是双排 2×21P 母座，插一块 42Pin 的 ESP32-S3-DevKitC-1 核心板。核心板模组是 ESP32-S3-WROOM-1 N16R8，16 MB Flash 加 8 MB Octal PSRAM，实测芯片为 ESP32-S3 双核、silicon rev v0.2。

底板上与这套工程相关的东西只有三样：一颗 WS2812 RGB 灯、一片 MAX98357 I2S 功放、一片 INMP441 数字麦克风。其余是传感器排针（已用于 07）、OLED 母座（已用于 08）、GPS 和 4G 模块位（4G 已用于 09）。

## 4G 模组（ML307R）

底板上的 Cat.1 模组实测型号 `ML307R-DL-MBRH0S01`，固件默认波特率 **115200**，供电必须 5 V（`VCC` 与 `EN` 都接 `5VIN`，模组常使能）。

串口按**模组侧**命名：底板网络 `G12_RXD` 是模组的 RX、`G11_TXD` 是模组的 TX。所以 ESP 侧要 **TX=GPIO12、RX=GPIO11**，反过来一个字节都收不到。这一点已用 `09_ml307_probe` 的双向自动探测实测确认。

不插 SIM 卡时，AT 通道照常可用：`AT+CGMR` 能读型号、`AT+CGMI` 类查询正常、IMEI 可读；`AT+CPIN?` 返回 `CME error 10`（无卡）、`AT+COPS?` 返回 `CME error 30`（无服务）、`AT+CSQ` 读到 99（无信号）。这些都是模组的正常业务应答，不是通信故障。

## 前三个工程用到的引脚

| 工程 | 外设 | 引脚 | 说明 |
|---|---|---|---|
| 01_blink_led | WS2812 RGB | GPIO48 | 底板丝印 `G48`，单灯，数据线直连 |
| 02_speaker_tone | MAX98357 | BCLK=GPIO15、LRC=GPIO16、DIN=GPIO7 | 功放 `SD` 接 3V3 常开，`GAIN` 接 GND |
| 03_mic_level | INMP441 | WS=GPIO4、SCK=GPIO5、SD=GPIO6 | 麦克风 `L/R` 接 GND，选择左声道 |

三组引脚互不重叠，可以同时启用。

## 上电前必须知道的三件事

第一，GPIO35、36、37 在 N16R8 模组上被 Octal PSRAM 占用，底板上虽然引到了排针，实际不可用。同理 GPIO26 到 32 归 Flash。外接电路前先对照 `docs/board.md` 的 GPIO 占用总表。

第二，GPIO0、3、45、46 是 strapping 脚，上电瞬间的电平决定启动模式。GPIO0 已经接了 BOOT 按键，外接电路不要在上电时把它强拉低，否则芯片会进下载模式而不是运行程序。GPIO46 接了日志输出，外接负载会干扰启动。

第三，GPIO43、44 是默认串口控制台，底板把这两个脚同时引给了 GPS 模块位。想用 GPS 就得先关掉控制台或改用 USB-CDC，否则两边抢同一组 UART。

## 供电

5V 从 `+5V` 经板上拨动开关进 `5VIN`，再由核心板 LDO 转 3V3 供给各模块。如果直接把 5VIN 短接到 LDO 输出侧，开关就形同虚设。功放和麦克风都吃 3V3，功耗不大，USB 供电足够。

## 两组 I2S 是独立的

这是最容易踩的坑。麦克风走 GPIO4/5/6，功放走 GPIO7/15/16，两组时钟线各自独立。它们不共用 BCLK/LRC，所以用同一个 I2S 控制器同时录音和放音是做不到的。ESP32-S3 有两个 I2S 外设，各占一个就能同时收发，`05_clap_echo` 正是这么做的：麦克风占一个控制器做接收，功放占另一个做发送，两者采样率可以不同（16 kHz 和 24 kHz）。另一条路是改硬件，把功放的时钟线并到麦克风那一组共时钟，代价是两边采样率必须一致。

## 两条 I2C 总线不要混接

底板上有两处 I2C：传感器总线是 GPIO8/GPIO9，接 AHT20 和 MPU6050；排针上的 I2C 是 GPIO41/GPIO42，给 OLED 用。两者是不同总线，接错会互相干扰。
