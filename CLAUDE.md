# 2048-game — agent context

Stack: esp32-device, C++ (ESP-IDF framework via PlatformIO), target board JC4880P443C_I_W.

Read [FUNCTIONAL_DESCRIPTION.md](FUNCTIONAL_DESCRIPTION.md) first — it is the living spec for
this project and both the local LLM and Claude Code should keep it in sync with actual behavior
as the project evolves.

Read [docs/BRINGUP.md](docs/BRINGUP.md) before touching hardware init code (display, PSRAM,
touch, WiFi, flashing) — it documents verified gotchas for this exact board. Getting any of
these wrong causes silent crashes or a board that never boots, not a compile error.

## Conventions

- Do not change the chip-revision, PSRAM, or display-init sequence in `sdkconfig.defaults` /
  `src/main.cpp` without reading `docs/BRINGUP.md` first — several of those settings look
  redundant or wrong but are required workarounds for this board's quirks.
- Secrets (WiFi credentials, API keys) belong in NVS or a gitignored config, never hardcoded in
  `src/main.cpp` for anything beyond initial bring-up testing.
- Build with the VS Code "Build" task, flash with "Upload", view logs with "Monitor" (or
  "Build + Upload + Monitor" for all three). Run "Test" before considering a change done, if
  tests exist for it.
- Local-first workflow: routine implementation is expected to happen via the local LLM
  (LM Studio + Continue). Escalate to Claude Code for architecture decisions, hard bugs, or
  security-sensitive changes.
- Commit and push manually (or run the "Sync" task) after meaningful changes.
