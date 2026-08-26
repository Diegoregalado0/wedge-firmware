/* RM67162 AMOLED over QSPI, for the LilyGO T-Display-S3 AMOLED 1.91".

   Pins follow LilyGo's own BOARD_AMOLED_191 definition. The panel speaks the
   usual QSPI display protocol, where every transaction is a one-byte mode
   prefix followed by the MIPI command in the middle of a 24-bit address:

     0x02 <cmd> for register writes, on one line
     0x32 <cmd> for pixel writes, on all four

   esp_lcd's SPI panel IO sends whatever is in lcd_cmd_bits as the leading
   phase, so the prefix and the command are packed into one 32-bit value rather
   than being written by hand. Getting this wrong is the classic failure where
   the panel initializes and then shows nothing. */

#include "rm67162.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "rm67162";

#define PIN_D0 18
#define PIN_D1 7
#define PIN_D2 48
#define PIN_D3 5
#define PIN_SCK 47
#define PIN_CS 6
#define PIN_RST 17
#define PIN_TE 9
#define PIN_EN 38

#define LCD_HOST SPI2_HOST
#define LCD_CLK_HZ (75 * 1000 * 1000)

#define QSPI_WRITE_CMD(c) ((uint32_t)0x02000000u | ((uint32_t)(c) << 8))
#define QSPI_WRITE_COLOR(c) ((uint32_t)0x32000000u | ((uint32_t)(c) << 8))

#define CMD_SLPIN 0x10
#define CMD_SLPOUT 0x11
#define CMD_DISPOFF 0x28
#define CMD_DISPON 0x29
#define CMD_CASET 0x2A
#define CMD_RASET 0x2B
#define CMD_RAMWR 0x2C
#define CMD_TEON 0x35
#define CMD_MADCTL 0x36
#define CMD_COLMOD 0x3A
#define CMD_WRDISBV 0x51
#define CMD_WRCTRLD 0x53

/* The glass is natively 240 wide by 536 tall. This bit pattern swaps and
   mirrors the scan so the controller itself delivers landscape, rather than
   the CPU transposing 128 kB every frame. */
#define MADCTL_LANDSCAPE 0x60

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_flush_done;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t tx(uint8_t cmd, const void *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, (int)QSPI_WRITE_CMD(cmd), data, len);
}

esp_err_t rm67162_init(void)
{
    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done, ESP_ERR_NO_MEM, TAG, "sem");

    const gpio_config_t outs = {
        .pin_bit_mask = (1ULL << PIN_EN) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outs), TAG, "gpio");
    gpio_set_level(PIN_EN, 1);

    const spi_bus_config_t bus = {
        .data0_io_num = PIN_D0,
        .data1_io_num = PIN_D1,
        .data2_io_num = PIN_D2,
        .data3_io_num = PIN_D3,
        .sclk_io_num = PIN_SCK,
        .max_transfer_sz = RM67162_W * RM67162_BAND * 2 + 16,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_CLK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io), TAG, "panel io");

    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    tx(CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t colmod = 0x55; /* 16 bits per pixel, RGB565 */
    tx(CMD_COLMOD, &colmod, 1);
    const uint8_t madctl = MADCTL_LANDSCAPE;
    tx(CMD_MADCTL, &madctl, 1);
    const uint8_t te = 0x00;
    tx(CMD_TEON, &te, 1);
    const uint8_t ctrld = 0x28; /* enable the brightness control block */
    tx(CMD_WRCTRLD, &ctrld, 1);

    /* Come up dark and let the app ramp the level, so a power cut at 3am does
       not flash a full-brightness panel into a dark bedroom. */
    rm67162_set_brightness(0);
    tx(CMD_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "panel up, %dx%d", RM67162_W, RM67162_H);
    return ESP_OK;
}

void rm67162_set_brightness(uint8_t level)
{
    /* The panel's own emission register, not a PWM backlight. An AMOLED has no
       backlight, which is why a black pixel at level 200 is still off and the
       night faces cost almost nothing to display. */
    tx(CMD_WRDISBV, &level, 1);
}

void rm67162_sleep(bool on)
{
    if (on) {
        tx(CMD_DISPOFF, NULL, 0);
        tx(CMD_SLPIN, NULL, 0);
    } else {
        tx(CMD_SLPOUT, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        tx(CMD_DISPON, NULL, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
}

void rm67162_blit_rows(uint16_t *pixels, int y0, int rows)
{
    const uint8_t ca[4] = { 0, 0, (uint8_t)((RM67162_W - 1) >> 8), (uint8_t)((RM67162_W - 1) & 0xFF) };
    const uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                            (uint8_t)((y0 + rows - 1) >> 8), (uint8_t)((y0 + rows - 1) & 0xFF) };
    tx(CMD_CASET, ca, 4);
    tx(CMD_RASET, ra, 4);

    /* wg_to_rgb565 hands back each pixel as a value, R in the high bits, and
       says nothing about byte order: on the S3 that value sits in memory low
       byte first. The panel wants it the other way round, high byte first on
       the wire, so left alone every pixel arrives with its two bytes
       transposed. That does not scramble the picture, since the shapes and
       text are still legible, it just picks the wrong 16 bits apart into red,
       green and blue, which is what a picture that is recognizable but the
       wrong color is a sign of. */
    int n = RM67162_W * rows;
    for (int i = 0; i < n; i++) {
        pixels[i] = (uint16_t)((pixels[i] << 8) | (pixels[i] >> 8));
    }

    esp_lcd_panel_io_tx_color(s_io, (int)QSPI_WRITE_COLOR(CMD_RAMWR), pixels,
                              (size_t)RM67162_W * rows * 2);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}
