/* CST816T capacitive touch over I2C.

   The controller reports a single finger and raises an interrupt on change.
   Only the raw point is read here: gesture recognition lives in core/ so the
   simulator and the device recognize gestures with the same code, and so the
   drag can be tracked continuously rather than waiting for the chip's own
   coarse gesture register to report a completed swipe. */

#include "cst816.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cst816";

#define PIN_SDA 3
#define PIN_SCL 2
#define PIN_IRQ 21
#define CST816_ADDR 0x15
#define REG_STATUS 0x01

static i2c_master_dev_handle_t s_dev;
static volatile bool s_irq;

static void IRAM_ATTR irq_isr(void *arg)
{
    s_irq = true;
}

esp_err_t cst816_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t handle;
    esp_err_t err = i2c_new_master_bus(&bus, &handle);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(handle, &dev, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    const gpio_config_t irq = {
        .pin_bit_mask = (1ULL << PIN_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&irq);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_IRQ, irq_isr, NULL);

    ESP_LOGI(TAG, "touch ready");
    return ESP_OK;
}

bool cst816_read(cst816_point_t *out)
{
    /* Polled every frame regardless of the interrupt: the flag only says
       something changed, and a finger held still stops producing edges while
       still very much being a finger on the glass. */
    uint8_t reg = REG_STATUS;
    uint8_t buf[6] = { 0 };
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 50) != ESP_OK) {
        return false;
    }
    s_irq = false;

    uint8_t fingers = buf[1] & 0x0F;
    if (fingers == 0) {
        out->pressed = false;
        return true;
    }

    /* Measured directly against the panel rather than assumed: five taps at
       the corners and center, logged raw alongside the screen position they
       landed at, showed the controller already reports in the panel's own
       landscape frame. raw_x tracked left-to-right 0..~520 against a 536px
       width and raw_y tracked top-to-bottom 40..~230 against a 240px height,
       both directly, no swap and no flip. The previous code assumed a native
       portrait chip and rotated for it, which was wrong for this board: it
       took the left-right axis, inverted it, and folded it into a 0..239
       range meant for the other axis, so anything past the middle of the
       screen produced a negative, unreachable y and only a narrow strip on
       the left ever hit anything. */
    uint16_t raw_x = (uint16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
    uint16_t raw_y = (uint16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
    out->pressed = true;
    out->x = (int16_t)raw_x;
    out->y = (int16_t)raw_y;
    return true;
}
