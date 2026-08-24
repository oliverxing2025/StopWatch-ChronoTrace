#include "display.h"

#include "config.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LCD_HOST SPI2_HOST
#define LCD_PCLK GPIO_NUM_40
#define LCD_DATA0 GPIO_NUM_41
#define LCD_DATA1 GPIO_NUM_42
#define LCD_DATA2 GPIO_NUM_46
#define LCD_DATA3 GPIO_NUM_45
#define LCD_CS GPIO_NUM_39

#define LCD_PANEL_X_GAP 6
#define LCD_CMD_BRIGHTNESS 0x51
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)

static const char *TAG = "display";
static DMA_ATTR uint16_t s_band_buf[2][BAND_PIXELS];
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_buffers_free;
static SemaphoreHandle_t s_brightness_lock;

// CO5300 sequence derived from M5Stack's official StopWatch ESP-IDF/M5GFX HAL.
static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD2}, 2, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xA0}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x11, NULL, 0, 150},
    {0x29, NULL, 0, 0},
};

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *event,
                                    void *user_ctx)
{
    (void)io;
    (void)event;
    (void)user_ctx;
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_buffers_free, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

esp_err_t display_init(void)
{
    s_buffers_free = xSemaphoreCreateCounting(2, 2);
    s_brightness_lock = xSemaphoreCreateMutex();
    if (s_buffers_free == NULL || s_brightness_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK, LCD_DATA0, LCD_DATA1, LCD_DATA2, LCD_DATA3,
        BAND_PIXELS * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config,
                                            SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(LCD_CS, on_trans_done, NULL);
    io_config.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                 &io_config, &s_io),
        TAG, "panel IO init failed");

    const co5300_vendor_config_t vendor_config = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_config,
                                                  &s_panel),
                        TAG, "panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG,
                        "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG,
                        "panel setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_PANEL_X_GAP, 0),
                        TAG, "panel gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG,
                        "panel on failed");

    ESP_LOGI(TAG, "CO5300 ready: %dx%d, QSPI 80MHz, x-gap %d",
             LCD_H_RES, LCD_V_RES, LCD_PANEL_X_GAP);
    return ESP_OK;
}

uint16_t *display_acquire_band(void)
{
    xSemaphoreTake(s_buffers_free, portMAX_DELAY);
    static unsigned next;
    return s_band_buf[next++ & 1U];
}

esp_err_t display_flush_band(int band_index, const uint16_t *buffer)
{
    const int y0 = band_index * BAND_ROWS;
    if (y0 >= LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }
    int y1 = y0 + BAND_ROWS;
    if (y1 > LCD_V_RES) {
        y1 = LCD_V_RES;
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_H_RES, y1, buffer);
}

esp_err_t display_set_brightness(uint8_t level)
{
    if (s_io == NULL || s_buffers_free == NULL || s_brightness_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // CO5300 parameter commands must not overlap a QSPI pixel DMA. Borrow both
    // band tokens: once the second token arrives, all queued transfers have
    // completed and the command can be sent reliably.
    xSemaphoreTake(s_brightness_lock, portMAX_DELAY);
    xSemaphoreTake(s_buffers_free, portMAX_DELAY);
    xSemaphoreTake(s_buffers_free, portMAX_DELAY);
    const esp_err_t err = esp_lcd_panel_io_tx_param(s_io, LCD_CMD_BRIGHTNESS,
                                                     &level, 1);
    xSemaphoreGive(s_buffers_free);
    xSemaphoreGive(s_buffers_free);
    xSemaphoreGive(s_brightness_lock);
    return err;
}
