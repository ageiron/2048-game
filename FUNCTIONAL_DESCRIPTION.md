# Functional Description — 2048-game

> **Living document.** Update this file whenever scope, requirements, or functionality change.
> Both the developer and any coding agent (Claude Code, local LLM) should treat this as the
> single source of truth for what the project is supposed to do.

- **Created:** 2026-08-11
- **Last updated:** 2026-08-11
- **Slug:** 2048-game
- **Target platform:** Guition JC4880P443C_I_W (ESP32-P4 + ESP32-C6)
- **Primary language(s):** C++ (ESP-IDF framework)

## Purpose

A single-player 2048 puzzle game running on the device's touchscreen. Swipe
up/down/left/right on the 4×4 board to slide and merge tiles; reach 2048 to
win. Tap "New Game" to reset. Score is shown top-right; the game does not
persist score/best-score across reboots. An "Auto Play" toggle lets the
device play itself using a heuristic solver (see Non-functional requirements).

## Hardware

See [docs/BRINGUP.md](docs/BRINGUP.md) for the full hardware reference table and verified
bring-up gotchas. Summary: ESP32-P4 + ESP32-C6 co-processor, 4.3" 480×800 MIPI-DSI ST7701
display, GT911 touch, 16 MB flash, 32 MB PSRAM.

## Framework / stack

- **Build system:** PlatformIO + [pioarduino/platform-espressif32](https://github.com/pioarduino/platform-espressif32) (stable release)
- **Framework:** ESP-IDF 5.5.x (downloaded automatically by PlatformIO on first build, ~1 GB)
- **UI library:** LVGL 9.2+ (managed component `lvgl/lvgl`)
- **Display driver:** ST7701 vendor files in `src/` (BSP does not support this panel — see `docs/BRINGUP.md`)
- **WiFi:** ESP-HOSTED via `espressif/esp_hosted` + `espressif/esp_wifi_remote` managed components
  (remove if this project doesn't need WiFi — see `docs/BRINGUP.md`)

## External APIs / integrations

TBD.

## WiFi credentials

TBD — plan to use NVS provisioning rather than hardcoding credentials in source.

## Build & flash

```
pio run                    # build only
pio run --target upload    # build + flash (see docs/BRINGUP.md for upload caveats)
pio device monitor         # serial console at 115200 baud
```

ESP-IDF toolchain (~1 GB) downloads automatically on the first `pio run`.

## Key source files

| File                        | Purpose                                           |
|-----------------------------|---------------------------------------------------|
| `platformio.ini`            | PlatformIO env, platform, board, build flags, custom upload command |
| `sdkconfig.defaults`        | ESP-IDF Kconfig defaults (flash, PSRAM, display, WiFi, LVGL) |
| `partitions.csv`            | Flash partition table (16 MB, factory app at 0x20000) |
| `src/main.cpp`              | App entry point (`app_main`): display, touch, UI |
| `src/CMakeLists.txt`        | ESP-IDF component registration + ST7701 source list |
| `src/idf_component.yml`     | IDF managed component dependencies (lvgl, esp_hosted, BSP) |
| `src/esp_lcd_st7701*.c/.h`  | ST7701 MIPI-DSI driver (vendor files, see `docs/BRINGUP.md`) |
| `docs/BRINGUP.md`           | Verified hardware bring-up gotchas — read before editing init code |

## Known gotchas

See [docs/BRINGUP.md](docs/BRINGUP.md) — kept separate from this file because it's
board-specific (applies to every project on this template) rather than project-specific.

## Non-functional requirements

- Single-finger swipe gestures via LVGL's built-in gesture recognizer
  (`LV_EVENT_GESTURE` / `lv_indev_get_gesture_dir`) — no extra input library.
  The listener must be on the screen object, not the board: every object with
  a parent gets LVGL's default `GESTURE_BUBBLE` flag, so the event bubbles
  past any sub-widget listener up to the first ancestor without it.
- Auto-play solver (`src/main.cpp`, `choose_best_move`/`expected_score`/
  `score_board`): 2-ply expectimax (own move → every possible random tile
  spawn → best reply) scored by a heuristic combining corner/monotonicity
  weighting, open-cell count, and neighbour smoothness. Runs from an LVGL
  timer (400 ms tick) inside the LVGL lock, so search depth is deliberately
  capped at 2 plies to avoid stalling screen redraws on this MCU.
- No WiFi/network dependency for gameplay (ESP-HOSTED stack is still wired up
  from the template but unused; see `docs/BRINGUP.md` if removing it later).

## Success criteria

- Swiping in all 4 directions correctly slides/merges tiles per standard 2048
  rules (verified in `src/main.cpp`'s `slide_line`/`move_board`).
- Game-over and win states are detected and shown; "New Game" resets cleanly.

## Out of scope

- Score persistence (NVS/best-score), animations/tile-slide transitions,
  undo, and multiplayer/network features.

## Change log

- 2026-08-11 — Project created from jc4880p443c-template.
- 2026-08-11 — Implemented 2048 gameplay in `src/main.cpp`: 4×4 LVGL board,
  swipe-gesture input, score tracking, win/game-over detection, New Game button.
- 2026-08-11 — Added Auto Play: heuristic 2-ply expectimax solver with a
  corner/openness/smoothness scoring function and a toggle button.
