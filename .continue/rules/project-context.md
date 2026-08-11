---
name: Project Context
alwaysApply: true
description: Points every session at this project's living spec and conventions before any work starts
---

Before doing ANY work in this session — even a small edit — read these files in the
project root first:

- `FUNCTIONAL_DESCRIPTION.md` — the living spec: purpose, stack, current scope, external APIs,
  success criteria. Update it if scope or behavior changes.
- `CLAUDE.md` — stack summary and conventions.
- `docs/BRINGUP.md` — verified hardware gotchas for the JC4880P443C_I_W board (chip revision,
  PSRAM, display init, touch, flashing). Getting any of this wrong causes silent crashes, not
  compile errors — read it before editing anything in `app_main()` or `sdkconfig.defaults`.

Do not skip this, even for requests that sound self-contained. Starting a fresh conversation
without this context is how work gets redone or contradicted. If something looks stale compared
to the actual code, say so and update it rather than silently ignoring the mismatch.
