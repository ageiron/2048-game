/**
 * 2048-game — JC4880P443C_I_W
 *
 * 2048 for the touchscreen: swipe up/down/left/right on the board to slide
 * and merge tiles. Tap "New Game" to reset.
 *
 * The block in app_main() up through touch init is the verified bring-up
 * sequence for this exact board — see docs/BRINGUP.md before touching it.
 *
 * Hardware: Guition JC4880P443C_I_W (ESP32-P4 + ESP32-C6)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
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
// 2048 game
// =============================================================================

#define GRID_N      4
#define CELL_SIZE   100
#define CELL_GAP    8
#define BOARD_PAD   8
#define BOARD_SIZE  (BOARD_PAD * 2 + CELL_SIZE * GRID_N + CELL_GAP * (GRID_N - 1))
#define BOARD_Y     170

static uint16_t g_board[GRID_N][GRID_N];
static lv_obj_t *g_cells[GRID_N][GRID_N];
static lv_obj_t *g_cell_labels[GRID_N][GRID_N];
static lv_obj_t *g_score_lbl;
static lv_obj_t *g_status_lbl;
static int g_score = 0;
static bool g_game_over = false;

static uint32_t tile_bg_color(uint16_t v) {
    switch (v) {
        case 0:    return 0xCDC1B4;
        case 2:    return 0xEEE4DA;
        case 4:    return 0xEDE0C8;
        case 8:    return 0xF2B179;
        case 16:   return 0xF59563;
        case 32:   return 0xF67C5F;
        case 64:   return 0xF65E3B;
        case 128:  return 0xEDCF72;
        case 256:  return 0xEDCC61;
        case 512:  return 0xEDC850;
        case 1024: return 0xEDC53F;
        case 2048: return 0xEDC22E;
        default:   return 0x3C3A32;
    }
}

static uint32_t tile_fg_color(uint16_t v) {
    return (v == 2 || v == 4) ? 0x776E65 : 0xF9F6F2;
}

static void spawn_tile(void) {
    int empty_r[GRID_N * GRID_N], empty_c[GRID_N * GRID_N], n = 0;
    for (int r = 0; r < GRID_N; r++)
        for (int c = 0; c < GRID_N; c++)
            if (g_board[r][c] == 0) { empty_r[n] = r; empty_c[n] = c; n++; }
    if (n == 0) return;
    int i = esp_random() % n;
    g_board[empty_r[i]][empty_c[i]] = (esp_random() % 10 == 0) ? 4 : 2;
}

// Slides a line of 4 toward index 0, merging equal neighbours once each.
// Adds each merge's value to *score_delta (if given). Returns true if the
// line changed (used to detect a legal move).
static bool slide_line(uint16_t line[GRID_N], int *score_delta = NULL) {
    uint16_t out[GRID_N] = {0};
    int n = 0;
    for (int i = 0; i < GRID_N; i++) if (line[i] != 0) out[n++] = line[i];
    for (int i = 0; i < n - 1; i++) {
        if (out[i] != 0 && out[i] == out[i + 1]) {
            out[i] *= 2;
            if (score_delta) *score_delta += out[i];
            for (int j = i + 1; j < n - 1; j++) out[j] = out[j + 1];
            out[n - 1] = 0;
            n--;
        }
    }
    bool changed = false;
    for (int i = 0; i < GRID_N; i++) {
        if (line[i] != out[i]) changed = true;
        line[i] = out[i];
    }
    return changed;
}

// Maps a (k, i) line coordinate to a board (row, col) for the given swipe direction.
static void line_to_board(lv_dir_t dir, int k, int i, int *r, int *c) {
    switch (dir) {
        case LV_DIR_LEFT:  *r = k; *c = i; break;
        case LV_DIR_RIGHT: *r = k; *c = GRID_N - 1 - i; break;
        case LV_DIR_TOP:   *r = i; *c = k; break;
        default:           *r = GRID_N - 1 - i; *c = k; break; // LV_DIR_BOTTOM
    }
}

// Applies dir to the given board array (any board, not just g_board) so it
// can be reused to simulate candidate moves for the auto-play heuristic.
static bool move_board_on(uint16_t board[GRID_N][GRID_N], lv_dir_t dir, int *score_delta = NULL) {
    bool changed = false;
    for (int k = 0; k < GRID_N; k++) {
        uint16_t line[GRID_N];
        int r, c;
        for (int i = 0; i < GRID_N; i++) {
            line_to_board(dir, k, i, &r, &c);
            line[i] = board[r][c];
        }
        if (slide_line(line, score_delta)) changed = true;
        for (int i = 0; i < GRID_N; i++) {
            line_to_board(dir, k, i, &r, &c);
            board[r][c] = line[i];
        }
    }
    return changed;
}

static bool move_board(lv_dir_t dir) {
    int delta = 0;
    bool changed = move_board_on(g_board, dir, &delta);
    g_score += delta;
    return changed;
}

static bool board_stuck(void) {
    for (int r = 0; r < GRID_N; r++)
        for (int c = 0; c < GRID_N; c++) {
            if (g_board[r][c] == 0) return false;
            if (c < GRID_N - 1 && g_board[r][c] == g_board[r][c + 1]) return false;
            if (r < GRID_N - 1 && g_board[r][c] == g_board[r + 1][c]) return false;
        }
    return true;
}

static void redraw_board(void) {
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            uint16_t v = g_board[r][c];
            lv_obj_set_style_bg_color(g_cells[r][c], lv_color_hex(tile_bg_color(v)), LV_PART_MAIN);
            if (v == 0) {
                lv_label_set_text(g_cell_labels[r][c], "");
            } else {
                lv_label_set_text_fmt(g_cell_labels[r][c], "%u", v);
                lv_obj_set_style_text_color(g_cell_labels[r][c], lv_color_hex(tile_fg_color(v)), LV_PART_MAIN);
            }
        }
    }
    lv_label_set_text_fmt(g_score_lbl, "Score: %d", g_score);
}

static void new_game(void) {
    memset(g_board, 0, sizeof(g_board));
    g_score = 0;
    g_game_over = false;
    lv_label_set_text(g_status_lbl, "");
    spawn_tile();
    spawn_tile();
    redraw_board();
}

static void new_game_btn_cb(lv_event_t *e) {
    (void)e;
    new_game();
}

// Applies dir if it's a legal move: slides/merges, spawns a tile, redraws,
// and updates game-over/win state. Returns whether the move actually happened.
static bool try_move(lv_dir_t dir) {
    if (g_game_over) return false;
    if (!move_board(dir)) return false;

    spawn_tile();
    redraw_board();

    bool won = false;
    for (int r = 0; r < GRID_N && !won; r++)
        for (int c = 0; c < GRID_N; c++)
            if (g_board[r][c] >= 2048) { won = true; break; }

    if (won) {
        g_game_over = true;
        lv_label_set_text(g_status_lbl, "You win! Tap New Game");
    } else if (board_stuck()) {
        g_game_over = true;
        lv_label_set_text(g_status_lbl, "Game Over! Tap New Game");
    }
    return true;
}

static void board_gesture_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    lv_indev_wait_release(indev); // one move per swipe

    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT && dir != LV_DIR_TOP && dir != LV_DIR_BOTTOM) return;
    try_move(dir);
}

// ---- Auto-play: 1-ply heuristic — simulate all 4 moves, keep the highest-scoring board ----
static const lv_dir_t AUTOPLAY_DIRS[] = { LV_DIR_LEFT, LV_DIR_BOTTOM, LV_DIR_RIGHT, LV_DIR_TOP };
static bool g_autoplay = false;
static lv_obj_t *g_autoplay_btn_lbl;

// Snake-pattern corner weights (biggest at top-left, winding through the
// grid) reward keeping large tiles cornered along a monotonic path — a
// standard cheap 2048 heuristic. Combined with a big bonus per empty cell.
static const int32_t AUTOPLAY_WEIGHTS[GRID_N][GRID_N] = {
    { 15, 14, 13, 12 },
    {  8,  9, 10, 11 },
    {  7,  6,  5,  4 },
    {  0,  1,  2,  3 },
};

static int32_t score_board(uint16_t board[GRID_N][GRID_N]) {
    int32_t score = 0;
    int32_t smoothness = 0; // sum of |log2 diff| between orthogonal neighbours
    int empty = 0;
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            uint16_t v = board[r][c];
            if (v == 0) { empty++; continue; }
            int exp = 31 - __builtin_clz((unsigned)v); // v is always a power of 2
            score += exp * AUTOPLAY_WEIGHTS[r][c]; // max ~17*15=255 per cell
            if (c < GRID_N - 1 && board[r][c + 1] != 0)
                smoothness += LV_ABS(exp - (31 - __builtin_clz((unsigned)board[r][c + 1])));
            if (r < GRID_N - 1 && board[r + 1][c] != 0)
                smoothness += LV_ABS(exp - (31 - __builtin_clz((unsigned)board[r + 1][c])));
        }
    }
    score += empty * 200; // open space is the #1 predictor of survival — keep
                           // this comparable in scale to the corner term above,
                           // not swamped by it
    score -= smoothness * 8; // penalize clutter between mismatched neighbours
                              // so merges stay available; weight kept modest
                              // so it can't outweigh corner/openness above
    return score;
}

// Leaf evaluator for the extra ply below: the best score_board() reachable
// with one more move from this board (or score_board() itself if no move is
// legal, i.e. this board is a terminal/stuck state).
static int32_t best_immediate_score(uint16_t board[GRID_N][GRID_N]) {
    int32_t best = -1;
    for (size_t i = 0; i < sizeof(AUTOPLAY_DIRS) / sizeof(AUTOPLAY_DIRS[0]); i++) {
        uint16_t copy[GRID_N][GRID_N];
        memcpy(copy, board, sizeof(copy));
        if (!move_board_on(copy, AUTOPLAY_DIRS[i])) continue;
        int32_t s = score_board(copy);
        if (s > best) best = s;
    }
    return best >= 0 ? best : score_board(board);
}

// 2-ply expectimax: for every possible random tile spawn (2 @ 90%, 4 @ 10%,
// in each empty cell) after our move, score the BEST reply we'd have to it —
// not just the raw post-spawn board — then average over spawns. Catches
// moves that look fine now but leave no good reply to an unlucky spawn.
static int32_t expected_score(uint16_t board[GRID_N][GRID_N]) {
    int empty_r[GRID_N * GRID_N], empty_c[GRID_N * GRID_N], n = 0;
    for (int r = 0; r < GRID_N; r++)
        for (int c = 0; c < GRID_N; c++)
            if (board[r][c] == 0) { empty_r[n] = r; empty_c[n] = c; n++; }
    if (n == 0) return best_immediate_score(board);

    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        uint16_t *cell = &board[empty_r[i]][empty_c[i]];
        *cell = 2;
        total += (int64_t)best_immediate_score(board) * 9; // 90% chance of a 2
        *cell = 4;
        total += (int64_t)best_immediate_score(board) * 1; // 10% chance of a 4
        *cell = 0;
    }
    return (int32_t)(total / (n * 10));
}

static lv_dir_t choose_best_move(void) {
    lv_dir_t best_dir = LV_DIR_NONE;
    int32_t best_score = -1;
    for (size_t i = 0; i < sizeof(AUTOPLAY_DIRS) / sizeof(AUTOPLAY_DIRS[0]); i++) {
        uint16_t copy[GRID_N][GRID_N];
        memcpy(copy, g_board, sizeof(copy));
        if (!move_board_on(copy, AUTOPLAY_DIRS[i])) continue; // illegal move
        int32_t s = expected_score(copy);
        if (s > best_score) { best_score = s; best_dir = AUTOPLAY_DIRS[i]; }
    }
    return best_dir;
}

static void set_autoplay(bool on) {
    g_autoplay = on;
    lv_label_set_text(g_autoplay_btn_lbl, on ? "Auto Play: On" : "Auto Play: Off");
}

static void autoplay_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!g_autoplay) return;
    if (g_game_over) { set_autoplay(false); return; }
    lv_dir_t dir = choose_best_move();
    if (dir != LV_DIR_NONE) try_move(dir);
}

static void autoplay_btn_cb(lv_event_t *e) {
    (void)e;
    set_autoplay(!g_autoplay);
}

static void create_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFAF8EF), LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // Gestures bubble up through every object's default GESTURE_BUBBLE flag and
    // land on the first ancestor without it — normally the screen — so the
    // listener belongs here, not on the board object itself.
    lv_obj_add_event_cb(scr, board_gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "2048");
    lv_obj_set_style_text_color(title, lv_color_hex(0x776E65), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 24);

    g_score_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(g_score_lbl, lv_color_hex(0x776E65), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_score_lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(g_score_lbl, LV_ALIGN_TOP_RIGHT, -24, 34);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 150, 50);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, -85, 90);
    lv_obj_add_event_cb(btn, new_game_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "New Game");
    lv_obj_center(btn_lbl);

    lv_obj_t *autoplay_btn = lv_btn_create(scr);
    lv_obj_set_size(autoplay_btn, 150, 50);
    lv_obj_align(autoplay_btn, LV_ALIGN_TOP_MID, 85, 90);
    lv_obj_add_event_cb(autoplay_btn, autoplay_btn_cb, LV_EVENT_CLICKED, NULL);
    g_autoplay_btn_lbl = lv_label_create(autoplay_btn);
    lv_label_set_text(g_autoplay_btn_lbl, "Auto Play: Off");
    lv_obj_center(g_autoplay_btn_lbl);

    lv_obj_t *board = lv_obj_create(scr);
    lv_obj_remove_flag(board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(board, BOARD_SIZE, BOARD_SIZE);
    lv_obj_align(board, LV_ALIGN_TOP_MID, 0, BOARD_Y);
    lv_obj_set_style_bg_color(board, lv_color_hex(0xBBADA0), LV_PART_MAIN);
    lv_obj_set_style_border_width(board, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(board, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(board, 0, LV_PART_MAIN);

    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            lv_obj_t *cell = lv_obj_create(board);
            lv_obj_remove_flag(cell, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            lv_obj_set_size(cell, CELL_SIZE, CELL_SIZE);
            lv_obj_set_pos(cell, BOARD_PAD + c * (CELL_SIZE + CELL_GAP), BOARD_PAD + r * (CELL_SIZE + CELL_GAP));
            lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(cell, 6, LV_PART_MAIN);

            lv_obj_t *lbl = lv_label_create(cell);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32, LV_PART_MAIN);
            lv_obj_center(lbl);

            g_cells[r][c] = cell;
            g_cell_labels[r][c] = lbl;
        }
    }

    g_status_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(g_status_lbl, lv_color_hex(0x776E65), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_status_lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(g_status_lbl, LV_ALIGN_TOP_MID, 0, BOARD_Y + BOARD_SIZE + 20);

    new_game();
    lv_timer_create(autoplay_timer_cb, 400, NULL);
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
