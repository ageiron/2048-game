/**
 * 2048-game — JC4880P443C_I_W bring-up starter
 *
 * Minimal proof-of-life app: brings up PSRAM, the ST7701 MIPI-DSI display,
 * LVGL, and GT911 touch, then shows a screen with a touch-reactive counter.
 * Tap the button to confirm touch works; watch the counter/label to confirm
 * the display and LVGL refresh loop work.
 *
 * Replace create_ui() (and everything below it) with your actual app.
 * The block above it (app_main up through touch init) is verified working
 * bring-up sequence for this exact board — see docs/BRINGUP.md before
 * touching it.
 *
 * Hardware: Guition JC4880P443C_I_W (ESP32-P4 + ESP32-C6)
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_lcd_st7701.h"
#include "esp_psram.h"
#include "esp_private/esp_psram_extram.h"

static const char *TAG = "2048-game";

// ---- Hardware ----------------------------------------------------------------
#define LCD_H_RES             480
#define LCD_V_RES             800
#define MIPI_DPI_PX_FORMAT    LCD_COLOR_PIXEL_FORMAT_RGB565
#define MIPI_DSI_PHY_LDO_CHAN 3
#define MIPI_DSI_PHY_LDO_MV  2500
#define PIN_BACKLIGHT         GPIO_NUM_23
#define PIN_LCD_RESET         GPIO_NUM_5
#define TP_I2C_SDA            GPIO_NUM_7
#define TP_I2C_SCL            GPIO_NUM_8

// =============================================================================
// UI — replace with your app
// =============================================================================

static lv_obj_t *g_counter_lbl;
static int g_tap_count = 0;

static void btn_event_cb(lv_event_t *e) {
    (void)e;
    g_tap_count++;
    lv_label_set_text_fmt(g_counter_lbl, "Taps: %d", g_tap_count);
}

static void create_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x181825), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "2048-game");
    lv_obj_set_style_text_color(title, lv_color_hex(0xCDD6F4), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "JC4880P443C_I_W bring-up OK");
    lv_obj_set_style_text_color(sub, lv_color_hex(0xA6E3A1), LV_PART_MAIN);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 110);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 200, 80);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Tap me");
    lv_obj_center(btn_lbl);

    g_counter_lbl = lv_label_create(scr);
    lv_label_set_text(g_counter_lbl, "Taps: 0");
    lv_obj_set_style_text_color(g_counter_lbl, lv_color_hex(0xCDD6F4), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_counter_lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(g_counter_lbl, LV_ALIGN_CENTER, 0, 120);
}

// =============================================================================
// Entry point — verified bring-up sequence, see docs/BRINGUP.md before editing
// =============================================================================

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== booting ===");

    // SPIRAM_BOOT_INIT=n: vendor bootloader already ran HW init; call explicitly
    // to map PSRAM into virtual address space and register it with the heap.
    esp_err_t psram_ret = esp_psram_init();
    if (psram_ret == ESP_OK) {
        ESP_ERROR_CHECK(esp_psram_extram_add_to_heap_allocator());
        ESP_LOGI(TAG, "PSRAM: %u B", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        ESP_LOGE(TAG, "esp_psram_init: %s", esp_err_to_name(psram_ret));
    }

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // LDO3 → MIPI DSI PHY power (must be first)
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));

    // Backlight off until panel is ready
    gpio_config_t bk = { .pin_bit_mask = 1ULL << PIN_BACKLIGHT, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&bk));
    gpio_set_level(PIN_BACKLIGHT, 0);

    // MIPI DSI bus: 2 lanes @ 500 Mbps
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = ST7701_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &mipi_dsi_bus));

    // DBI command channel
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = ST7701_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_cfg, &io_handle));

    // DPI: 34 MHz, 480×800, RGB565
    esp_lcd_dpi_panel_config_t dpi_cfg = ST7701_480_360_PANEL_60HZ_DPI_CONFIG(MIPI_DPI_PX_FORMAT);
    st7701_vendor_config_t vendor_cfg = {
        .mipi_config = { .dsi_bus = mipi_dsi_bus, .dpi_config = &dpi_cfg },
        .flags       = { .use_mipi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_dev_cfg = {
        .reset_gpio_num = PIN_LCD_RESET,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io_handle, &panel_dev_cfg, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    gpio_set_level(PIN_BACKLIGHT, 1);
    ESP_LOGI(TAG, "ST7701 initialised");

    // LVGL
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = io_handle,
        .panel_handle   = panel_handle,
        .control_handle = NULL,
        .buffer_size    = LCD_H_RES * 100,
        .double_buffer  = true,
        .hres           = LCD_H_RES,
        .vres           = LCD_V_RES,
        .color_format   = LV_COLOR_FORMAT_RGB565,
        .flags          = { .buff_spiram = true },
    };
    const lvgl_port_display_dsi_cfg_t dsi_disp_cfg = { .flags = { .avoid_tearing = 0 } };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_disp_cfg);
    ESP_ERROR_CHECK(disp == NULL ? ESP_FAIL : ESP_OK);

    // GT911 touch
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = TP_I2C_SDA,
        .scl_io_num        = TP_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags             = { .enable_internal_pullup = true },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    // NOTE: fields set in struct declaration order, NOT via the
    // ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() macro — see docs/BRINGUP.md
    // "GT911 I2C config macro out-of-order".
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr            = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 0,
        .lcd_cmd_bits        = 16,
        .lcd_param_bits      = 8,
        .flags               = { .disable_control_phase = 1 },
        .scl_speed_hz        = 100000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_handle_t touch_handle = NULL;
    esp_lcd_touch_config_t touch_cfg = {
        .x_max        = LCD_H_RES,
        .y_max        = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels       = { .reset = 0, .interrupt = 0 },
        .flags        = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &touch_cfg, &touch_handle));

    const lvgl_port_touch_cfg_t touch_port_cfg = { .disp = disp, .handle = touch_handle };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_port_cfg);
    ESP_ERROR_CHECK(indev == NULL ? ESP_FAIL : ESP_OK);

    // Build UI
    lvgl_port_lock(0);
    create_ui();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI ready");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Heap: %lu B  PSRAM: %lu B",
            (unsigned long)esp_get_free_heap_size(),
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
