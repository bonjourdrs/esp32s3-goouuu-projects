# ST7789 1.54 寸 240×240 LCD

用四线 SPI 驱动底板 8P 母座上的 ST7789 屏幕，先画彩条确认像素和方向，再进入文字、色块、移动方块和帧计数画面。

## 接线

8 针接口顺序已由用户确认：GND、VCC、SCL、SDA、RES、DC、CS、BLK。

| 屏幕 | ESP32-S3 |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SCL / SCK | GPIO21 |
| SDA / MOSI | GPIO47 |
| RES | GPIO45 |
| DC | GPIO40 |
| CS | GPIO41 |
| BLK | GPIO42 |

这里是四线 SPI，SDA/SCL 并非 I2C。接口不含 MISO，所以无法通过读屏幕 ID 确认连接。

## 行为

- 使用 ESP-IDF 自带 `esp_lcd` 的 ST7789 驱动：SPI3_HOST、mode 0、20 MHz、RGB565、反色开启、偏移 0/0。
- 主机像素是小端，驱动配置 `LCD_RGB_DATA_ENDIAN_LITTLE`。
- 上电先显示 8 段彩条 2 秒，然后进入主画面：标题 `ESP32 S3 240X240`、RGB 色块、移动方块、`FRAME xxxxx` 帧计数和完整白边。
- 每 50 帧打印一行 `frame=... DMA OK`。

## 验证现象

彩条颜色顺序为红、绿、蓝、黄、青、品红、白、黑，铺满 240×240。随后画面四边应有一圈完整白边，用来观察偏移和裁切；移动方块应平滑横移，帧计数持续增长。

串口每 50 帧输出：

```
I (2600) LCD_DEMO: frame=50 DMA OK
```

`DMA OK` 只证明传输完成，颜色、方向和裁切仍需目视确认。

## 运行

```powershell
.\tools\build.ps1   -Project 08_st7789_lcd
.\tools\flash.ps1   -Project 08_st7789_lcd -Port COM4
.\tools\monitor.ps1 -Project 08_st7789_lcd -Port COM4
```

## 文件结构

| 文件 | 作用 |
|---|---|
| `main/main.c` | SPI 总线与面板初始化、绘图、帧循环 |

## 调参

- 显示偏移：`main.c` 的 `OFFSET_X` / `OFFSET_Y`，默认 0/0。
- 红蓝对调：调整 `rgb_ele_order`。
- 刷新速度：`SPI_CLOCK_HZ` 默认 20 MHz，屏幕花屏时先降频。
- 分辨率：`WIDTH` / `HEIGHT` 默认 240×240。

## 注意

黑屏时按顺序检查供电、8P 针序和背光（BLK=GPIO42）；花屏优先降 SPI 频率；颜色异常先看 `rgb_ele_order` 和 `LCD_RGB_DATA_ENDIAN_LITTLE`。

参考实现：

- 引脚配置：https://github.com/78/xiaozhi-esp32/blob/main/main/boards/bread-compact-wifi-lcd/config.h
- 面板初始化：https://github.com/78/xiaozhi-esp32/blob/main/main/boards/bread-compact-wifi-lcd/compact_wifi_board_lcd.cc
