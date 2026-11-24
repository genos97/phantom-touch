# Phantom Touch — PC-Side C++11 Multi-Touch Keymapper

A lightweight, native Win32 C++11 mouse & keyboard multi-touch keymapper and visual HUD Studio built around official `scrcpy`.
**Zero Android code is written or installed on your phone** — all raw mouse capture, 360° virtual-finger camera control, independent X/Y sensitivity physics, WASD joystick glide, hotkey combos, and Keymap Studio UI run 100% on Windows.

> **Important Notice for Online Games:** Using external mouse/keyboard keymappers in competitive mobile games may violate a game's Terms of Service and risk account restrictions. Use responsibly.

---

## Project Architecture

```text
phantom-touch/
├── src/
│   ├── app/
│   │   ├── main.cpp                       # Entry point
│   │   └── Application.hpp / .cpp         # CLI bootstrap, DPI setup, lifecycle orchestration
│   ├── bridge/
│   │   ├── TouchTypes.hpp                 # TouchAction enum, logical PointerId channels, TouchEmitFn sink
│   │   ├── TouchFingerPool.hpp / .cpp     # Bounded Android 0..9 virtual finger allocator & FIFO slot rotator
│   │   ├── SpsParser.hpp / .cpp           # H.264 Annex-B SPS RBSP resolution parser (auto-rotation)
│   │   └── ScrcpyBridge.hpp / .cpp        # Official scrcpy v2-v5 ADB/TCP bridge & display launcher
│   ├── config/
│   │   ├── KeyCodes.hpp / .cpp            # VK <-> name, combos, modifier side normalization, mouse/scroll VKs
│   │   ├── Profile.hpp / .cpp             # KeyNode, Profile schema, Free Fire & blank defaults
│   │   └── ProfileStore.hpp / .cpp        # .cfg load/save, profile discovery, last-active persistence
│   ├── engine/
│   │   ├── FpsViewController.hpp / .cpp   # Standalone FpsViewFinger + FpsViewStack (360° camera pointer)
│   │   ├── JoystickController.hpp / .cpp  # Virtual WASD joystick, 3-stage stepped glide, Sprint/Walk
│   │   ├── ActionDispatcher.hpp / .cpp    # 2-level hotkey combos + 10 KeyActionType runners
│   │   └── InputEngine.hpp / .cpp         # Non-blocking hook queue, QPC polling loop, fire modes, telemetry
│   └── ui/
│       ├── GdiDraw.hpp / .cpp             # Shared Win32 GDI drawing primitives
│       ├── HudRenderer.hpp / .cpp         # Play/Edit HUD badges, floating CFG ball, status bar, prompts
│       ├── StudioPanel.hpp / .cpp         # Draggable 4-tab Keymap Studio & Key Inspector (Mouse + Scroll UI)
│       └── OverlayWindow.hpp / .cpp       # Topmost layered window, WM_INPUT Raw Mouse, WH_MOUSE/KEYBOARD_LL
├── tests/
│   └── test_core.cpp                      # Unit tests covering bridge, config, engine, and UI modules
├── profiles/
│   └── freefire.cfg                       # Calibrated Free Fire MAX profile
├── build.bat                              # Zero-dependency MinGW C++11 build script
├── run.bat                                # One-click launcher
└── setup_scrcpy.ps1                       # Official scrcpy Windows downloader
```

---

## Key Features

- **Standalone 360° Virtual-Finger FPS Controller (`FpsViewController`):** Lazy touch-down, exact `double` sub-pixel accumulation, Free Fire unbounded mode (`ff_counter_accel`) + boxed 360° edge-release (`>=15 ms` non-blocking re-plant with alternating pointer IDs & `±50 px` jitter), idle auto-recenter, and a 2-level `FpsViewStack` for Free Look Eye (`NODE_HOLD_VIEW`).
- **Non-Blocking Keyboard + Mouse Concurrency (`InputEngine`):** `WH_KEYBOARD_LL` and `WH_MOUSE_LL` enqueue into a microsecond lock without ever blocking on TCP `send()`. Keyboard & `WASD` remain fully usable even when the mouse cursor is unlocked.
- **Bounded Virtual Finger Pool (`TouchFingerPool`):** Dynamically allocates Android multi-touch slots `0..9` with FIFO round-robin rotation on camera re-plants, overflow protection, duplicate-DOWN/orphan-UP guards, and complete stuck-finger drain on focus loss.
- **Full Mouse & Scroll Wheel Support:** Bind `LMB`, `RMB`, `MMB`, `X1`, `X2`, `SCROLL UP`, `SCROLL DOWN`, keyboard keys, or 2-key combos (`LALT+1`) to any of 10 action types (`PRESS`, `CLICK`, `DOWN_UP`, `UP_TRIGGER`, `HOLD_VIEW`, `SWIPE`, `CURSOR_TOGGLE`, `RESET_VIEW`, `SWITCH_SENS`, `REACTIVATE_JOY`). In unlocked Cursor Mode, scrolling over the mirror smoothly touch-drags lists/backpacks (`80 ms` auto-release) even inside Unity/Unreal games.
- **Visual Keymap Studio (`F2` or Floating `CFG` Ball):** Drag keys anywhere, double-click any key to replace it, tune X/Y/Fire/Scope sensitivity sliders live, and create/clone/switch saved `.cfg` profiles.

---

## Quick Start

### 1. Download Official `scrcpy` (One-Time Setup)
```powershell
powershell -ExecutionPolicy Bypass -File .\setup_scrcpy.ps1
```

### 2. Build from Source (MinGW `g++`, Zero External Dependencies)
```bat
build.bat
```

### 3. Run
```powershell
# Live Android device over USB debugging:
.\run.bat

# Offline Keymap Studio demo (no phone required):
.\scrcpy_keymapper.exe --demo
```

---

## Hotkeys

| Hotkey | Action |
| :--- | :--- |
| **`~`** (configurable) or **`F1`** | Toggle **Aim Mode** (locked 360° Raw Mouse) ↔ **Cursor Mode** (free cursor; keyboard & WASD stay active) |
| **`F2`** or **`CFG` Ball** | Open / Close **Visual Keymap Studio** |
| **`F3`** | Cycle Fire Mode (`CLAW-LEFT` / `DRAG-FIRE` / `DUAL-DRAG`) |
| **`F4`** | Show / Hide HUD key badges while playing |
| **`F5`** | Quick reset all virtual touches |
| **`[` / `]`** & **`-` / `=`** | Live adjust X / Y sensitivity (`±0.05`) |
| **Double-Click Key** *(in F2)* | Prompt to replace key (Keyboard / Mouse / Scroll Up / Scroll Down / Combo) |
| **`Ctrl + S`** *(in F2)* | Save active profile to disk |
