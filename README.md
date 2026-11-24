# Phantom Touch v2.0 — Production Emulator Shell & Multi-Touch Keymapper

A native Windows C++11 / GDI+ **Android Emulator Shell** and multi-touch keyboard & mouse keymapper built around official `scrcpy 5.0`.
The phone screen runs **embedded inside** Phantom Touch's dark frameless desktop window — with every control, Keymap Studio, Key Inspector, Sensitivity slider, Profile Manager, Android navigation bar, and USB/Wireless ADB Device Hub cleanly docked **outside** the phone screen.

**Zero Android code is written or installed on your phone** — all raw mouse capture, 360° virtual-finger camera control, bounded `0..9` multi-touch allocation, WASD joystick glide, hotkey combos, and the Emulator UI run 100% natively on Windows (`< 15 MB` RAM, zero console window).

> **Important Notice for Online Games:** Using external mouse/keyboard keymappers in competitive mobile games may violate a game's Terms of Service and risk account restrictions. Use responsibly.

---

## Project Architecture

```text
phantom-touch/
├── resources/
│   ├── phantom_touch.ico                # Multi-size dark cyan Windows app icon
│   ├── app.manifest                     # Per-Monitor V2 DPI awareness manifest
│   └── phantom_touch.rc                 # Icon, manifest, and VERSIONINFO resource script
├── src/
│   ├── app/
│   │   ├── main.cpp                     # GUI entry point (-mwindows, optional --console)
│   │   └── Application.hpp / .cpp       # Single-instance guard, DPI v2, GDI+ session, shell bootstrap
│   ├── bridge/
│   │   ├── TouchTypes.hpp               # TouchAction enum, logical PointerId channels, TouchEmitFn sink
│   │   ├── TouchFingerPool.hpp / .cpp   # Bounded Android 0..9 virtual finger allocator & FIFO rotator
│   │   ├── SpsParser.hpp / .cpp         # H.264 Annex-B SPS RBSP resolution parser (auto-rotation)
│   │   └── ScrcpyBridge.hpp / .cpp      # Embedded borderless scrcpy 5.0 launcher, ADB Device Hub, screenshots
│   ├── config/
│   │   ├── KeyCodes.hpp / .cpp          # VK <-> name, combos, modifier normalization, mouse/scroll VKs
│   │   ├── Profile.hpp / .cpp           # KeyNode, Profile schema, Free Fire & blank defaults
│   │   ├── ProfileStore.hpp / .cpp      # Atomic .cfg save/load, buttons_explicit, last-active persistence
│   │   └── AppSettings.hpp / .cpp       # Window geometry, maximized state, stream FPS/bitrate/audio prefs
│   ├── engine/
│   │   ├── FpsViewController.hpp / .cpp # Standalone FpsViewFinger + FpsViewStack (360° camera pointer)
│   │   ├── JoystickController.hpp / .cpp# Virtual WASD joystick, FF 4-step & fast 3-step glide, Sprint/Walk
│   │   ├── ActionDispatcher.hpp / .cpp  # 2-level hotkey combos + 10 KeyActionType runners + add-on effects
│   │   └── InputEngine.hpp / .cpp       # Non-blocking hook queue, QPC polling loop, fire modes, cursor scroll
│   └── ui/
│       ├── Theme.hpp                    # Obsidian & electric-cyan emulator theme tokens & layout metrics
│       ├── GdiDraw.hpp / .cpp           # Lightweight GDI helpers for HudOverlay
│       ├── GdiPlusDraw.hpp / .cpp       # Anti-aliased GDI+ cards, toggles, sliders, vector icons, aspect-fit
│       ├── TopBar.hpp / .cpp            # Frameless header, device pill, profile switcher, live telemetry
│       ├── SideRail.hpp / .cpp          # 54px right emulator action rail + Android Back/Home/Recents/Vol
│       ├── ViewportHost.hpp / .cpp      # Embedded scrcpy container (SetParent), auto-rotation, Device Hub
│       ├── DrawerPanel.hpp / .cpp       # 356px docked right Studio Drawer (Keys/Inspector, Sens, Profiles, Device)
│       ├── HudOverlay.hpp / .cpp        # Lightweight owned overlay pinned strictly to the phone video rect
│       ├── HudRenderer.hpp / .cpp       # Clean HUD key badges, crosshair, toasts, drag-node visuals
│       └── ShellWindow.hpp / .cpp       # Master frameless emulator frame, F11 fullscreen, hooks, auto-save
├── tests/
│   └── test_core.cpp                    # Automated unit tests across bridge, config, engine, and shell UI
├── profiles/
│   └── freefire.cfg                     # Calibrated Free Fire MAX profile
├── build.bat                            # Compiles resources + PhantomTouch.exe (-mwindows -O2 -Wall -Wextra)
├── build_release.bat                    # Stages clean portable dist/PhantomTouch/ release bundle
├── run.bat                              # One-click GUI launcher (no console window)
└── setup_scrcpy.ps1                     # Official scrcpy Windows downloader
```

---

## Key Features

- **Emulator-Style Desktop Shell (`ShellWindow` + `ViewportHost`):** Runs borderless `scrcpy 5.0` embedded directly inside a frameless dark desktop window with automatic portrait ↔ landscape aspect-ratio letterboxing, `F11` fullscreen gaming mode, Always-on-Top pin, and zero console window clutter.
- **Outside-Viewport Studio (`DrawerPanel` + `SideRail`):** Every editing tool (10-node Keymap Palette, Key Inspector, X/Y/Fire/Scope Sensitivity sliders, Profiles Manager, Device/Stream Settings, Activity Log, and Android Back/Home/Recents/Vol/Screenshot controls) sits cleanly **outside** the phone screen so nothing blocks your game HUD.
- **USB & Wireless ADB Device Hub:** Automatically scans plugged-in phones, supports Wireless ADB (`IP:Port`), live stream configuration (`60/90/120 FPS`, `8M–24M` bitrate, audio forwarding, screen-off), and 1-click Offline Keymap Studio.
- **Standalone 360° Virtual-Finger FPS Engine + Bounded `0..9` Pool:** Preserves ultra-smooth Free Fire counter-acceleration physics, non-blocking keyboard+mouse concurrency, active WASD/keyboard while cursor is unlocked, smooth unlocked backpack scroll-drag, and instant atomic auto-save on every edit.

---

## Quick Start

### 1. Download Official `scrcpy` (One-Time Setup)
```powershell
powershell -ExecutionPolicy Bypass -File .\setup_scrcpy.ps1
```

### 2. Build from Source (MinGW `g++` + `windres`, Zero External Dependencies)
```bat
build.bat
```
To create a clean portable distribution folder (`dist\PhantomTouch\`):
```bat
build_release.bat
```

### 3. Run
```powershell
# Launch Emulator Shell (auto-connects USB phone or opens Device Hub):
.\run.bat

# Open directly in Offline Keymap Studio (no phone required):
.\PhantomTouch.exe --demo

# Optional developer console for stdout debugging:
.\PhantomTouch.exe --console
```

---

## Hotkeys

| Hotkey | Action |
| :--- | :--- |
| **`~`** (configurable) or **`F1`** | Toggle **Aim Lock** (360° Raw Mouse) ↔ **Cursor Mode** (keyboard & WASD stay active) |
| **`F2`** or **`KEYS` Rail Button** | Open / Close **Outside-Viewport Keymap Studio** |
| **`F3`** or **`FIRE` Rail Button** | Cycle Fire Mode (`CLAW-LEFT` / `DRAG-FIRE` / `DUAL-DRAG`) |
| **`F4`** or **`HUD` Rail Button** | Show / Hide HUD key badges while playing |
| **`F5`** or **`RST` Rail Button** | Quick reset all virtual touches |
| **`F11`** or **`FULL` Rail Button** | Toggle **Immersive Fullscreen Mode** |
| **`[` / `]`** & **`-` / `=`** | Live adjust X / Y sensitivity (`±0.05`, auto-saved) |
| **Double-Click Key** *(in Keymap Studio)* | Replace key (Keyboard / Mouse / Scroll Up / Scroll Down / 2-Key Combo) |
| **`Ctrl + S`** *(in Keymap Studio)* | Force save profile (all edits also auto-save automatically) |
