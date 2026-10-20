# ST7789 1.54 寸 240×240 LCD demo

8 针接口顺序已由用户确认：GND、VCC、SCL、SDA、RES、DC、CS、BLK。

|屏幕|ESP32-S3|
|---|---|
|GND|GND|
|VCC|3V3|
|SCL/SCK|GPIO21|
|SDA/MOSI|GPIO47|
|RES|GPIO45|
|DC|GPIO40|
|CS|GPIO41|
|BLK|GPIO42|

这里是四线 SPI，SDA/SCL 并非 I2C。使用 ESP-IDF 自带 esp_lcd ST7789 驱动；SPI mode 0、20 MHz、RGB565、反色开启、偏移 0/0。主机像素采用小端，驱动配置 LCD_RGB_DATA_ENDIAN_LITTLE。

上电先显示彩条 2 秒，再显示 ST7789 / LCD DEMO、RGB 色块、移动方块和帧计数。完整白边用于观察偏移和裁切。串口 DMA OK 只证明传输完成，实际画面仍需目视确认。

构建：tools/build.ps1 -Project 08_st7789_lcd
烧录：tools/flash.ps1 -Project 08_st7789_lcd -Port COM4

参考小智配置：https://github.com/78/xiaozhi-esp32/blob/main/main/boards/bread-compact-wifi-lcd/config.h
参考初始化：https://github.com/78/xiaozhi-esp32/blob/main/main/boards/bread-compact-wifi-lcd/compact_wifi_board_lcd.cc

若显示偏移可调整 main.c 的 OFFSET_X/OFFSET_Y；若红蓝对调可调整 rgb_ele_order；黑屏时先核对供电、针序和背光。接口不含 MISO，不能通过读屏幕 ID 确认连接。
