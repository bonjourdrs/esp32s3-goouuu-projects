#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define WIDTH 240
#define HEIGHT 240
#define PIN_SCLK 21
#define PIN_MOSI 47
#define PIN_RST 45
#define PIN_DC 40
#define PIN_CS 41
#define PIN_BL 42
#define SPI_CLOCK_HZ (20 * 1000 * 1000)
#define OFFSET_X 0
#define OFFSET_Y 0
static const char *TAG = "LCD_DEMO";
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t transfer_done;
static uint16_t *pixels;

// Original compact 5x7 row patterns, ordered A-Z then 0-9.
static const uint8_t font[][7] = {
 {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
 {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
 {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
 {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
 {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
 {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
 {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
 {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
 {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
 {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
 {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
 {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
};
static uint16_t rgb(unsigned r, unsigned g, unsigned b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j=y; j<y+h; ++j)
        for (int i=x; i<x+w; ++i)
            if (i>=0 && i<WIDTH && j>=0 && j<HEIGHT) pixels[j*WIDTH+i]=c;
}
static void text_at(int x, int y, const char *s, int scale, uint16_t c)
{
    for (; *s; ++s, x+=6*scale) {
        int n = *s>='A' && *s<='Z' ? *s-'A' :
                (*s>='0' && *s<='9' ? 26+*s-'0' : -1);
        if (n<0) continue;
        for (int row=0; row<7; ++row)
            for (int col=0; col<5; ++col)
                if (font[n][row] & (1<<(4-col)))
                    rect(x+col*scale,y+row*scale,scale,scale,c);
    }
}
static bool color_done(esp_lcd_panel_io_handle_t io,
                       esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    BaseType_t awakened=pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &awakened);
    return awakened==pdTRUE;
}
static void present(void)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, WIDTH, HEIGHT, pixels));
    // DMA reads pixels asynchronously: never overwrite the buffer before completion.
    if (xSemaphoreTake(transfer_done,pdMS_TO_TICKS(2000))!=pdTRUE) {
        ESP_LOGE(TAG,"LCD DMA transfer timed out");
        abort();
    }
}
void app_main(void)
{
    ESP_LOGI(TAG,"ST7789 240x240 SPI: SCK=21 MOSI=47 RST=45 DC=40 CS=41 BL=42");
    gpio_config_t bl={.pin_bit_mask=1ULL<<PIN_BL,.mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&bl));
    ESP_ERROR_CHECK(gpio_set_level(PIN_BL,0));
    transfer_done=xSemaphoreCreateBinary();
    pixels=heap_caps_malloc(WIDTH*HEIGHT*sizeof(uint16_t),MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if (!transfer_done || !pixels) {
        ESP_LOGE(TAG,"Cannot allocate display buffer or semaphore");
        return;
    }
    spi_bus_config_t bus={
        .mosi_io_num=PIN_MOSI,.miso_io_num=-1,.sclk_io_num=PIN_SCLK,
        .quadwp_io_num=-1,.quadhd_io_num=-1,
        .max_transfer_sz=WIDTH*HEIGHT*sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST,&bus,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg={
        .cs_gpio_num=PIN_CS,.dc_gpio_num=PIN_DC,.spi_mode=0,
        .pclk_hz=SPI_CLOCK_HZ,.trans_queue_depth=2,
        .lcd_cmd_bits=8,.lcd_param_bits=8,
        .on_color_trans_done=color_done,.user_ctx=transfer_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST,&io_cfg,&io));
    esp_lcd_panel_dev_config_t cfg={
        .reset_gpio_num=PIN_RST,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian=LCD_RGB_DATA_ENDIAN_LITTLE,.bits_per_pixel=16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io,&cfg,&panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel,false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel,false,false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel,OFFSET_X,OFFSET_Y));
    memset(pixels,0,WIDTH*HEIGHT*sizeof(uint16_t));
    present();
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
    ESP_ERROR_CHECK(gpio_set_level(PIN_BL,1));
    ESP_LOGI(TAG,"Panel commands sent, backlight on; verify image visually");
    const uint16_t colors[]={0xf800,0x07e0,0x001f,0xffe0,0x07ff,0xf81f,0xffff,0x0000};
    for (int i=0;i<8;++i) rect(i*30,0,30,HEIGHT,colors[i]);
    present();
    vTaskDelay(pdMS_TO_TICKS(2000));
    unsigned frame=0;
    int x=24,dx=3;
    while (1) {
        uint16_t bg=rgb(12,18,30),white=rgb(240,245,255),cyan=rgb(0,220,220);
        rect(0,0,WIDTH,HEIGHT,bg);
        rect(0,0,WIDTH,2,white); rect(0,HEIGHT-2,WIDTH,2,white);
        rect(0,0,2,HEIGHT,white); rect(WIDTH-2,0,2,HEIGHT,white);
        text_at(18,14,"ST7789",3,cyan);
        text_at(18,43,"LCD DEMO",2,white);
        text_at(18,68,"ESP32 S3 240X240",2,rgb(150,170,200));
        rect(18,94,64,30,0xf800); rect(88,94,64,30,0x07e0); rect(158,94,64,30,0x001f);
        text_at(43,102,"R",2,white);text_at(113,102,"G",2,0);text_at(183,102,"B",2,white);
        rect(18,142,204,48,rgb(25,40,58));
        rect(x,152,24,28,cyan);
        char label[24];
        snprintf(label,sizeof(label),"FRAME %05u",frame%100000);
        text_at(18,204,label,2,white);
        present();
        if (frame%50==0) ESP_LOGI(TAG,"frame=%u DMA OK",frame);
        ++frame;
        x+=dx;
        if (x>=192 || x<=24) dx=-dx;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
