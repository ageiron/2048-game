# JC4880P443C_I_W bring-up notes

Everything in this file was learned the hard way while bringing up the
desk-helper project on this exact board (Guition JC4880P443C_I_W: ESP32-P4 +
ESP32-C6, 4.3" 480×800 MIPI-DSI ST7701 panel, GT911 touch). It's baked into
this template's `sdkconfig.defaults`, `platformio.ini`, `partitions.csv`, and
`src/main.cpp` already — this doc explains *why*, so you don't accidentally
undo a fix while customizing the app.

## Hardware reference

| Component      | Details                                                        |
|-----------------|------------------------------------------------------------------|
| Main SoC        | ESP32-P4, dual-core RISC-V @ 400 MHz, rev 1.x (ECO2)             |
| WiFi / BT       | ESP32-C6 co-processor via SDIO (ESP-HOSTED protocol)             |
| Display         | 4.3" IPS 480×800, MIPI-DSI, ST7701 controller                    |
| Touch           | Goodix GT911, I2C (SDA=GPIO7, SCL=GPIO8)                         |
| Flash           | 16 MB (DIO)                                                      |
| PSRAM           | 32 MB octal, 200 MHz                                             |
| Backlight       | GPIO23 (active-high, digital)                                    |
| LCD reset       | GPIO5                                                            |
| MIPI PHY power  | Internal LDO3, 2500 mV                                           |
| SDIO (P4↔C6)    | CLK=18, CMD=19, D0=14, D1=15, D2=16, D3=17, Reset=54              |

Full vendor spec sheet: `docs/JC4880P443C_I_W Specifications-EN-V1.0*.pdf`
(included in this template).

The vendor also ships a much larger SDK package (schematics, Arduino/IDF demo
examples, burn tool, ~550 MB) — not included here because of size. If you need
it, look for the original `JC4880P443C_I_W/` vendor folder from wherever you
sourced the board (Guition product page / included link), or ask whoever set
up the first project (desk-helper) for a copy. The ST7701 driver files in
`src/` of this template were extracted from
`JC4880P443C_I_W/1-Demo/arduino_examples/lvgl_demo_v8/src/lcd/` in that
package — you shouldn't need the rest of it for a new project.

## Toolchain

Use **pioarduino**, not the official PlatformIO espressif32 platform — the
official `platformio/espressif32` v7.x does not support ESP32-P4 at all.
`platformio.ini` in this template already points at:
```
https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
```
Board: `esp32-p4`. Framework: `espidf` (the `espidf` env is the proven path;
an `esp32p4_arduino` env is included as a fallback but WiFi via ESP-HOSTED has
limited Arduino support).

## Display bring-up

The Espressif BSP (`espressif/esp32_p4_function_ev_board`) **does not support
this board's 480×800 ST7701 panel** — it only handles 1024×600 and 1280×800.
Do not call BSP display init functions (`bsp_display_start`,
`bsp_display_new`, etc.) — they fail or produce garbage on this panel. It's
kept as a component dependency only because other managed components pull it
in transitively; sdkconfig options referencing it are Kconfig defaults for the
component, not something this template actually uses.

The display is driven **directly**, via the ST7701 vendor driver files in
`src/` (`esp_lcd_st7701.c/.h`, `esp_lcd_st7701_interface.h`,
`esp_lcd_st7701_mipi.c`, `esp_lcd_st7701_rgb.c` — the RGB variant must be
compiled even on this MIPI-only board because `SOC_LCD_RGB_SUPPORTED=y` on
ESP32-P4 and the driver's dispatch file references it unconditionally).

Init sequence (see `src/main.cpp`):
1. `esp_ldo_acquire_channel()` — power LDO3 at 2500 mV for the MIPI PHY
2. `esp_lcd_new_dsi_bus()` — 2-lane DSI bus at 500 Mbps
3. `esp_lcd_new_panel_io_dbi()` — DBI command channel
4. `esp_lcd_new_panel_st7701()` → `reset()` → `init()` — 480×800 DPI config at 34 MHz
5. GPIO23 high — backlight on
6. `lvgl_port_init()` + `lvgl_port_add_disp_dsi()` — register with LVGL
7. `i2c_new_master_bus()` + `esp_lcd_touch_new_i2c_gt911()` + `lvgl_port_add_touch()` — GT911 touch

## Chip revision selector (CRITICAL — crash on boot if wrong)

`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y` are
required in `sdkconfig.defaults`. This board uses ESP32-P4 rev 1.x (ECO2).
Rev <3.0 and >=3.0 have fundamentally different memory maps — selecting the
wrong one causes the ROM to zero out its SPI-flash data pointer
(`0x4ff3ffe8`) on startup and crash immediately, before any of your code runs.

## PSRAM not available in heap

`CONFIG_SPIRAM_BOOT_INIT` must be **unset** (not `y`) — the vendor bootloader
already initializes PSRAM, and letting the app re-init it from
`cpu_start.c` causes a null-dereference crash on startup.

But leaving it unset also means `cpu_start.c` never calls `esp_psram_init()`,
so PSRAM is detected but never mapped into virtual address space —
`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` returns 0.

**Fix** (already in `src/main.cpp`, top of `app_main()`):
```cpp
// SPIRAM_BOOT_HW_INIT=y already ran esp_psram_chip_init().
// Call esp_psram_init() explicitly to do the virtual-address mapping.
esp_err_t psram_ret = esp_psram_init();
if (psram_ret == ESP_OK) {
    ESP_ERROR_CHECK(esp_psram_extram_add_to_heap_allocator());
}
```
Gives ~31 MB of free PSRAM in the heap — required for LVGL draw buffers
(`buff_spiram = true` in `lvgl_port_display_cfg_t`).

**PSRAM mode**: ESP32-P4 only supports HEX (16-bit) PSRAM mode.
`CONFIG_SPIRAM_MODE_OCT=y` in `sdkconfig.defaults` is silently ignored;
`CONFIG_SPIRAM_MODE_HEX=y` is always what Kconfig actually selects. Don't
worry if you see HEX in the generated `sdkconfig` — that's expected.

## GT911 I2C config macro out-of-order

`ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()` initializes `scl_speed_hz` before
`control_phase_bytes`, which is the wrong order for the struct's declaration
order. GCC rejects this with `-Werror`. Initialize the
`esp_lcd_panel_io_i2c_config_t` struct manually instead of using the macro
(already done in `src/main.cpp`).

## Flash address / partition offset

The partition table (`partitions.csv`) puts the factory app at `0x20000`, not
the ESP-IDF/PlatformIO default `0x10000`. `platformio.ini`'s custom
`upload_command` overrides the esptool invocation to write bootloader,
partition table, and firmware at the correct offsets (`0x2000`, `0x8000`,
`0x20000`).

## esptool venv is broken in bundled PlatformIO

The PlatformIO-bundled esptool.py (in the `.espidf-5.5.x` venv) is missing
`rich_click`, and that venv has no `pip` to install it. Workaround (already
wired into `upload_command` in `platformio.ini`): use a system Python
esptool install instead.

Install it once per machine:
```
<python-install-dir>\python.exe -m pip install esptool
```
Then update the `upload_command` path in `platformio.ini` to point at that
Python's `Scripts\esptool.exe`.

**Unicode build failure**: PlatformIO can crash with `UnicodeEncodeError`
(cp1252 console) when printing the firmware size table after a successful
build. Exit code is 1, but the binaries ARE built correctly at
`.pio/build/esp32p4/{bootloader,partitions,firmware}.bin`. If `pio run
--target upload` fails this way, flash manually:
```powershell
& "<esptool-path>\esptool.exe" `
    --chip esp32p4 --port COM6 --baud 921600 `
    --before default-reset --after hard-reset `
    write-flash -z --flash-mode dio --flash-freq 80m --flash-size detect `
    0x2000  ".pio\build\esp32p4\bootloader.bin" `
    0x8000  ".pio\build\esp32p4\partitions.bin" `
    0x20000 ".pio\build\esp32p4\firmware.bin"
```

## Linker script gotcha after a clean sdkconfig regeneration

If `sdkconfig` is deleted and the first full rebuild fails at the linker step
(missing `_bss_start_low` / `_heap_start_low` — ESP32-P4 rev <3.0 low-memory
symbols), **do not** just re-run `pio run`. The partial build leaves an
incomplete linker script; relinking against it produces firmware that
corrupts startup memory and crash-loops. Always run `pio run --target clean`
before rebuilding after a failed sdkconfig regeneration.

## esp_hosted 3.x breaks the build on Windows/PlatformIO (CRITICAL — pin to 2.x)

`src/idf_component.yml` pins `espressif/esp_hosted` to `>=2.11,<3.0`, not `*`.
Leaving it unconstrained resolves to esp_hosted 3.0.6+ as of mid-2026, and
**every compile fails** with:
```
riscv32-esp-elf-g++: fatal error: cannot specify '-o' with '-c', '-S' or '-E' with multiple files
compilation terminated.
```

Root cause: esp_hosted's `host/eh_host_config/CMakeLists.txt` (from 3.0
onward) adds `"SHELL:-include eh_host_port_master_config.h"` as a compile
option. The `SHELL:` prefix is a CMake convention (for Ninja/Make generators)
meaning "don't split or reorder this token group." PlatformIO's flag
extraction for its scons-based ESP-IDF integration doesn't honor `SHELL:` on
Windows, so `-include` and its filename argument get separated by other
flags. GCC then reads `eh_host_port_master_config.h` as a second input
*source* file instead of the argument to `-include`, and rejects the
combination of multiple source files with a single `-o`/`-c`.

This isn't a template bug — it reproduces on a clean `esp_hosted@3.0.6` +
PlatformIO on Windows regardless of project. Confirmed non-flaky (fails
identically with `-j1`, single-threaded). Fix is simply staying on the 2.x
line, which doesn't use `SHELL:`, until PlatformIO or esp_hosted fixes the
incompatibility upstream. If you ever see this error after touching
`idf_component.yml` or deleting `dependencies.lock`, check the resolved
`espressif/esp_hosted` version in `dependencies.lock` first.

Note: `espressif/esp_wifi_remote` has no 2.x/3.x version line as of this
writing (latest is 1.6.x) — don't try to pin it the same way, it's
`esp_hosted`'s version that needs constraining. `esp_wifi_remote`'s own
manifest happens to privately pin `esp_hosted: '>=2.11,<3.0'` for the
esp32p4/esp32h2 targets in some releases, which is how desk-helper's original
lock file avoided this even with `esp_hosted: "*"` — but don't rely on that
transitive constraint staying in place in a future `esp_wifi_remote` release;
pin `esp_hosted` directly instead, as this template does.

## USB-JTAG is the only bidirectional PC↔device path

USB-JTAG (shows up as a COM port, e.g. COM6) is the only bidirectional path
between PC and ESP32-P4 on this board. UART0 TX bridges to USB, but UART0 RX
does **not** receive data sent from the PC — so `uart_read_bytes()` never
sees anything the host sends.

**Fix**: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults` (ESP-IDF
startup then auto-installs the driver before `app_main`). Call
`usb_serial_jtag_driver_install()` explicitly in `app_main` too — it returns
`ESP_ERR_INVALID_STATE` harmlessly if the console startup already did it. Read
incoming bytes with `usb_serial_jtag_read_bytes()` from a dedicated FreeRTOS
task (give it at least an 8192-byte stack).

If your project doesn't need the PC to send data to the device, you can drop
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` and just use ESP-IDF's normal log
output.

**COM port lock**: on Windows, stale processes (leftover Python agents, other
monitor sessions) can hold the COM port open and block flashing. Kill them
before flashing, e.g. `Stop-Process -Name python -Force`.

## WiFi (ESP-HOSTED via ESP32-C6)

ESP32-P4 has no built-in WiFi — this board's WiFi/BT goes through the
ESP32-C6 co-processor over SDIO, using Espressif's ESP-HOSTED managed
component (`espressif/esp_hosted` + `espressif/esp_wifi_remote`). GPIO wiring
is fixed by the board (see hardware reference table above) and matched by
`CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y` in `sdkconfig.defaults`.

If a project genuinely doesn't need WiFi, you can remove the whole
ESP-HOSTED block from `sdkconfig.defaults`, the `espressif__esp_hosted` /
`espressif__esp_wifi_remote` entries from `src/idf_component.yml` and
`src/CMakeLists.txt`'s `REQUIRES`, and the `esp_wifi`/`esp_netif`/`esp_event`
`REQUIRES` too.
