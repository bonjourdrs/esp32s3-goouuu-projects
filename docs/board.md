# 板卡信息 · ESP32-S3-DevKitC-1 42Pin GOOUUU 果云版

底板为双排 2×21P 母座结构，可插入 42Pin 的 ESP32-S3-DevKitC-1 核心板。
板载两路 I2C、两组 I2S、UART0/UART1 引出，并预留 7 个可选焊接模块位。
原理图见 [assets/schematic.png](assets/schematic.png)，可交互版见 [assets/board_reference.html](assets/board_reference.html)。

## 核心与供电

| 项 | 说明 |
|---|---|
| 核心板 | ESP32-S3-DevKitC-1 42Pin（果云版），插在底板 U1/U2 两组 21P 母座上 |
| 模组 | ESP32-S3-WROOM-1 **N16R8**：16 MB Quad SPI Flash + 8 MB Octal SPI PSRAM |
| 芯片 | ESP32-S3，双核，silicon rev v0.2 |
| 板载 LED | WS2812 RGB，接 **GPIO48** |
| BOOT 按键 | **GPIO0**（strapping） |
| 3V3 供电 | 由核心板 LDO 输出，引到 U2 第 2、21 脚 |
| 5V 供电 | `+5V`（充放电一体板）→ 电源开关 → `5VIN`（U1 第 21 脚） |
| USB | `G19_UD-` / `G20_UD+`（GPIO19/20），同时引到 U2 第 13、14 脚 |

## 排针 / 母座引脚定义

### U2 · 左排 21P（X6511FV-21-C85D32）

| # | 丝印网络 | 功能 |
|---|---|---|
| 1 | GND | 电源 |
| 2 | 3V3 | 电源 |
| 3 | ESP_EN | 核心板使能/复位 |
| 4 | G4_WS | I2S 麦克风 WS |
| 5 | G5_SCK | I2S 麦克风 SCK |
| 6 | G6_SD | I2S 麦克风 SD |
| 7 | G7_DIN | I2S 功放 DIN |
| 8 | G15_BCLK | I2S 功放 BCLK |
| 9 | G16_LRC | I2S 功放 LRC |
| 10 | G17_VOL+ | 音量+ 按键 |
| 11 | G18 | 通用 IO |
| 12 | G8 | I2C 传感器总线 |
| 13 | G19_UD- | USB D− |
| 14 | G20_UD+ | USB D+ |
| 15 | G43_JTAG | UART0 TX / JTAG |
| 16 | G46_LOG | 日志输出（strapping） |
| 17 | G9 | I2C 传感器总线 |
| 18 | G10 | 通用 IO |
| 19 | G11_TXD | UART1 TX |
| 20 | G12_RXD | UART1 RX |
| 21 | 3V3 | 电源 |

### U1 · 右排 21P（X6511FV-21-C85D32）

| # | 丝印网络 | 功能 |
|---|---|---|
| 1 | GND | 电源 |
| 2 | G1 | 通用 IO |
| 3 | G2 | 通用 IO |
| 4 | TX0 | UART0 TX（GPIO43） |
| 5 | RX0 | UART0 RX（GPIO44） |
| 6 | G42_SCL | I2C SCL |
| 7 | G41_SDA | I2C SDA |
| 8 | G40 | 通用 IO |
| 9 | G39_VOL- | 音量− 按键 |
| 10 | G38 | GPS 模块第 5 脚 |
| 11 | G37 | PSRAM 占用（不可用） |
| 12 | G36 | PSRAM 占用（不可用） |
| 13 | G35 | PSRAM 占用（不可用） |
| 14 | G0_BOOT | BOOT 按键（strapping） |
| 15 | G45 | strapping 脚 |
| 16 | G48 | 板载 WS2812 RGB LED |
| 17 | G47 | OLED 数据线 |
| 18 | G21 | OLED 数据线 |
| 19 | G14 | 通用 IO |
| 20 | G13 | 通用 IO |
| 21 | 5VIN | 电源 5V 输入 |

## 板载按键

| 按键 | 连接 | 说明 |
|---|---|---|
| 音量 + | GPIO17 ↔ GND | 按下拉低，需内部上拉 |
| 音量 − | GPIO39 ↔ GND | 按下拉低，需内部上拉 |
| BOOT | GPIO0 ↔ GND | 下载模式按键（strapping） |

## 板载 / 可选模块

| 模块 | 类型 | 引脚连接 |
|---|---|---|
| AHT20 + BMP280 | 温湿度 + 气压 · I2C | 3V3 / G8 / GND / G9（4P 排针位） |
| MPU6050 | 六轴陀螺仪 + 加速度计 · I2C | GND / 3V3 / G9 / G8（与 AHT20 共用 G8/G9） |
| **INMP441** | 数字 MEMS 麦克风 · I2S | L/R→GND、WS→G4、SCK→G5、SD→G6、VDD→3V3 |
| **MAX98357** | I2S 数字功放 · I2S | LRC→G16、BCLK→G15、DIN→G7、GAIN→GND、SD→3V3 |
| OLED 屏母座 | 8P 2.54mm · I2C/SPI | GND/3V3/G21/G47/G45/G40/G41_SDA/G42_SCL |
| ATGM336H | GPS / 北斗 · UART | GND / 3V3 / RX0(G44) / TX0(G43) / G38（5P 2.0mm） |
| ML307R-4G | Cat.1 4G 模块 · UART | EN→5VIN、RXD→G12、TXD→G11、VCC→5VIN |
| 外接电源开关 | SPDT 拨动开关 | 5VIN / +5V |

## GPIO 资源占用总表

| GPIO | 板载用途 | 排针位置 | 备注 |
|---|---|---|---|
| 0 | BOOT 按键 | U1-14 | strapping |
| 1 | — | U1-2 | 通用 IO |
| 2 | — | U1-3 | 通用 IO |
| 3 | — | 未引出 | strapping |
| 4 | INMP441 WS | U2-4 | I2S 麦克风 |
| 5 | INMP441 SCK | U2-5 | I2S 麦克风 |
| 6 | INMP441 SD | U2-6 | I2S 麦克风 |
| 7 | MAX98357 DIN | U2-7 | I2S 功放 |
| 8 | 传感器 I2C | U2-12 | AHT20 / MPU6050 |
| 9 | 传感器 I2C | U2-17 | AHT20 / MPU6050 |
| 10 | — | U2-18 | 通用 IO |
| 11 | ML307R TXD | U2-19 | UART1 |
| 12 | ML307R RXD | U2-20 | UART1 |
| 13 | — | U1-20 | 通用 IO |
| 14 | — | U1-19 | 通用 IO |
| 15 | MAX98357 BCLK | U2-8 | I2S 功放 |
| 16 | MAX98357 LRC | U2-9 | I2S 功放 |
| 17 | 音量+ 按键 | U2-10 | 按键到 GND |
| 18 | — | U2-11 | 通用 IO |
| 19 | USB D− | U2-13 | USB 数据线 |
| 20 | USB D+ | U2-14 | USB 数据线 |
| 21 | OLED | U1-18 | I2C/SPI 均可 |
| 26–32 | SPI Flash | 未引出 | Flash 专用 |
| 33–37 | Octal PSRAM | U1-11/12/13 | N16R8 下不可用 |
| 38 | ATGM336H 第 5 脚 | U1-10 | GPS 预留 |
| 39 | 音量− 按键 | U1-9 | 按键到 GND |
| 40 | OLED | U1-8 | SPI 数据线 |
| 41 | OLED SDA | U1-7 | I2C |
| 42 | OLED SCL | U1-6 | I2C |
| 43 | ATGM336H TX0 | U1-4 | UART0（默认控制台） |
| 44 | ATGM336H RX0 | U1-5 | UART0（默认控制台） |
| 45 | — | U1-15 | strapping |
| 46 | 日志 | U2-16 | strapping |
| 47 | OLED | U1-17 | SPI 数据线 |
| 48 | 板载 RGB LED | U1-16 | WS2812 单灯 |

## 接线与使用注意

1. **GPIO35 / 36 / 37 不可用** —— N16R8 使用 8MB Octal PSRAM，占用 GPIO33–37。
2. **UART0 与 GPS 冲突** —— ATGM336H 接 GPIO43/44，而这是默认串口控制台；同时使用需关闭控制台或改用 USB-CDC。
3. **ML307R 的 TX/RX 需核对** —— 原理图网络名以 ESP 侧命名，串口需交叉连接，实测确认后再定。
4. **麦克风与功放是两组独立 I2S** —— INMP441 用 G4/G5/G6，MAX98357 用 G7/G15/G16，时钟不共用；
   要用同一 I2S 控制器做「录 + 放」需改接引脚或使用两个 I2S 外设。
5. **两路 I2C 总线不要混接** —— 传感器总线 G8/G9（AHT20、MPU6050）；排针 I2C G41_SDA/G42_SCL（OLED）。
6. **strapping 脚与供电** —— GPIO0/3/45/46 为 strapping，外接电路不要在上电时强拉电平；
   5V 需经 `+5V → 电源开关 → 5VIN` 接入，若 5VIN 直接短接到 LDO，开关会失效。

## 本仓库三个工程用到的引脚

| 工程 | 外设 | 引脚 |
|---|---|---|
| 01_blink_led | WS2812 RGB | GPIO48 |
| 02_speaker_tone | MAX98357 | BCLK=15、LRC=16、DIN=7 |
| 03_mic_level | INMP441 + WS2812 | WS=4、SCK=5、SD=6、LED=48 |
