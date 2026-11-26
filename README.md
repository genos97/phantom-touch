# Phantom Touch v2.1 — Real-Phone Android Emulator Shell for Windows

**Use your real Android phone like a desktop emulator — with `< 15 MB` PC RAM, zero VM lag, your own accounts, and your phone's real GPU.**

Phantom Touch runs a native 64-bit Windows C++11 / **Direct3D 11** / **Media Foundation** emulator shell connected to your real Android phone via a **single `scrcpy-server` stream** (`scrcpy 5.0`). Whether you're using WhatsApp/Chrome on a low-end laptop, playing casual/strategy games, running an app on a **Second-Phone Virtual Display** while your physical phone stays free, or playing controller-supported games with an **XInput gamepad**, everything runs cleanly inside one dark frameless desktop frame.

**Zero Android APK is installed on your phone** — all H.264 hardware decoding, Direct3D 11 same-frame HUD compositing, PC typing & clipboard sync, Drag-and-Drop APK/file push, XInput gamepad-to-touch, and multi-touch keymapping run 100% natively on Windows.

> **Known Limits of Mirroring a Real Phone:**
> - Apps that set Android's `FLAG_SECURE` (banking apps, Netflix/DRM video) display a black screen while mirrored, and some banking apps refuse to run while Developer Options are enabled.
> - Competitive online shooters may restrict external keyboard/mouse mapping under their Terms of Service. Lead with everyday apps, casual/strategy games, and controller-friendly titles.

---

## What's Inside (Stages 1, 2 & 3 Complete)

### Stage 1 — Everyday Android App Emulator
- **Per-App Auto-Switching Profiles:** Automatically detects the open Android app (`adb shell dumpsys window/activity`) and switches to your linked game profile — or automatically enters clean **Desktop Typing Mode** when you open WhatsApp, Chrome, Notes, or the Home screen. Link any app with 1 click in `PROFILES`.
- **Full PC Typing & Bidirectional Copy-Paste:** Type directly into any Android text field (`INJECT_TEXT` + Android keycodes with Shift/Ctrl metastate) and use `Ctrl+C`, `Ctrl+V`, `Ctrl+X`, `Ctrl+A`, `Ctrl+Z` with live bidirectional Windows ↔ Android clipboard sync.
- **Cursor Mode Mouse Shortcuts:** `Ctrl + Mouse Wheel` performs smooth 2-finger pinch-zoom (strictly inside Android's `0..9` pointer pool), normal Wheel performs touch-scroll drag, and **Right-Click = Android Back** (toggleable in `DEVICE`).
- **Drag-and-Drop Install & Transfer:** Drop any `.apk` onto the Phantom Touch window to run `adb install -r`; drop any other file (image, PDF, video, zip) to push it straight into `/sdcard/Download/`.

### Stage 2 — Single-Connection Native Video Renderer (Media Foundation + Direct3D 11)
- **Single Phone Video Encoder:** Replaces `scrcpy.exe`'s PC video viewer while keeping `scrcpy-server` on the phone — cutting phone video encoders from 2 down to 1!
- **Hardware H.264 -> Direct3D 11 Same-Frame Compositor:** Decodes full-speed `60/90/120 FPS` H.264 via Windows built-in **Media Foundation** (`CMSH264DecoderMFT` -> NV12) and draws the phone screen + frosted-glass keymap HUD together in the **exact same Direct3D 11 frame** (no separate floating Win32 overlay window, no `SetParent` embedding, zero z-order glitches).
- **Instant Local PNG Screenshots:** Captures screenshots directly from the latest decoded frame into `screenshots\` (works in Virtual Display mode too!).
- **Optional Audio Forwarding:** When enabled in `DEVICE`, launches `scrcpy.exe --no-window --no-video --no-control` for audio-only playback.

### Stage 3 — Differentiating Real-Phone Emulator Features
- **"Second Phone" Virtual Display Mode:** Creates an independent PC-shaped virtual Android display (`1920x1080/320`, `1280x720/240`, or `Auto` via `scrcpy-server` `new_display` + `flex_display`) so apps run in Phantom Touch while your physical phone screen stays completely free!
- **Shareable Edge-Anchored Profiles (`20:9 <-> 16:9`):** Anchors left-edge and right-edge buttons relative to screen edges (scaling proportionally to screen height) so shared profiles survive aspect-ratio differences between 20:9 phones, 16:9 phones, and 16:9 Virtual Displays — with exact inverse edit-mode dragging.
- **Built-in XInput Gamepad-to-Touch (`GamepadController`):** Plug in any Xbox / XInput controller for 360° analog left-stick touch movement (with sprint threshold), quadratic right-stick 360° camera look, RT Fire / LT Scope, and face/shoulder/D-Pad button mapping.
- **Android 11+ Wireless QR Code Pairing (`QrCodeGen` + mDNS):** Click **QR PAIR** in the Device Hub to display a real scannable ISO/IEC 18004 QR code (`WIFI:T:ADB;S:...;P:...;;`) — scan it from *Developer options -> Wireless debugging -> Pair device with QR code* and Phantom Touch auto-discovers, pairs (`adb pair`), and connects wirelessly over Wi-Fi.

---

## Project Architecture

```text
phantom-touch/
├── resources/
│   ├── phantom_touch.ico                # Multi-size dark cyan Windows app icon
│   ├── app.manifest                     # Reference Per-Monitor V2 manifest
│   └── phantom_touch.rc                 # Icon & VERSIONINFO (2.1.0.0)
├── src/
│   ├── app/
│   │   ├── main.cpp                     # 64-bit GUI entry point (-mwindows, optional --console)
│   │   └── Application.hpp / .cpp       # Single-instance guard, Per-Monitor V2 DPI, GDI+ session
│   ├── bridge/
│   │   ├── TouchTypes.hpp               # TouchAction enum, PointerId channels (0..9), TouchEmitFn
│   │   ├── TouchFingerPool.hpp / .cpp   # Bounded Android 0..9 virtual finger allocator & FIFO rotator
│   │   ├── SpsParser.hpp / .cpp         # H.264 Annex-B SPS RBSP resolution parser (auto-rotation)
│   │   ├── QrCodeGen.hpp / .cpp         # Zero-dependency ISO/IEC 18004 QR encoder (Android 11+ ADB pair)
│   │   └── ScrcpyBridge.hpp / .cpp      # Single-connection scrcpy-server bridge, text/clipboard/app/APK/mDNS
│   ├── video/
│   │   ├── H264Decoder.hpp / .cpp       # Windows Media Foundation CMSH264DecoderMFT (Annex-B -> NV12)
│   │   └── D3D11Renderer.hpp / .cpp     # Direct3D 11 NV12->RGB shader + same-frame PARGB HUD + PNG capture
│   ├── config/
│   │   ├── KeyCodes.hpp / .cpp          # VK <-> name, Android keycodes, combos, modifier normalization
│   │   ├── Profile.hpp / .cpp           # Profile schema, Desktop/Free Fire presets, Edge-Anchored remapping
│   │   ├── ProfileStore.hpp / .cpp      # Atomic .cfg save/load, buttons_explicit, last-active persistence
│   │   └── AppSettings.hpp / .cpp       # Window geometry, D3D11/Virtual Display/Gamepad/Auto-Switch prefs
│   ├── engine/
│   │   ├── FpsViewController.hpp / .cpp # Standalone FpsViewFinger + FpsViewStack (360° camera pointer)
│   │   ├── JoystickController.hpp / .cpp# Virtual WASD joystick, FF 4-step & fast 3-step glide, Sprint/Walk
│   │   ├── GamepadController.hpp / .cpp # Dynamic XInput loader, 360° analog stick, right-stick look, triggers
│   │   ├── ActionDispatcher.hpp / .cpp  # 2-level hotkey combos + 10 KeyActionType runners + add-on effects
│   │   └── InputEngine.hpp / .cpp       # Non-blocking hook queue, QPC loop, pinch-zoom, desktop passthrough
│   └── ui/
│       ├── Theme.hpp                    # Obsidian & electric-cyan emulator theme tokens & layout metrics
│       ├── GdiPlusDraw.hpp / .cpp       # Anti-aliased GDI+ cards, toggles, sliders, vector icons, aspect-fit
│       ├── TopBar.hpp / .cpp            # Frameless header, [2ND] badge, profile pill, PAD/Typing telemetry
│       ├── SideRail.hpp / .cpp          # 54px right emulator action rail + Android Back/Home/Recents/Vol
│       ├── ViewportHost.hpp / .cpp      # Direct3D 11 video viewport + USB/Wireless/QR Device Hub
│       ├── DrawerPanel.hpp / .cpp       # 356px outside-viewport drawer (Keys, Sens, Profiles/App-Link, Device)
│       ├── HudOverlay.hpp / .cpp        # Same-frame D3D11 PARGB surface + offline studio layered fallback
│       ├── HudRenderer.hpp / .cpp       # Minimalist frosted-glass translucent keycaps & hover quick-delete
│       └── ShellWindow.hpp / .cpp       # Master shell: drag-drop, clipboard sync, app watcher, QR pairing
├── tests/
│   └── test_core.cpp                    # 10 automated test suites (Stages 1, 2 & 3 incl. live D3D11/MFT)
├── profiles/
│   └── freefire.cfg                     # Calibrated Free Fire MAX profile
├── build.bat                            # Builds 64-bit PhantomTouch.exe (WinLibs MinGW-w64 UCRT, 0 warnings)
├── build_release.bat                    # Stages clean portable dist/PhantomTouch/ release bundle
├── run.bat                              # One-click GUI launcher (no console window)
└── setup_scrcpy.ps1                     # Official scrcpy Windows downloader
```

---

## Quick Start

### 1. Download Official `scrcpy` (One-Time Setup)
```powershell
powershell -ExecutionPolicy Bypass -File .\setup_scrcpy.ps1
```

### 2. Build from Source (64-bit MinGW-w64 UCRT / WinLibs GCC, Zero External Libraries)
```bat
build.bat
```
To package a standalone portable folder (`dist\PhantomTouch\`):
```bat
build_release.bat
```

### 3. Run
```powershell
# Launch Emulator Shell (auto-connects USB phone, or opens USB / Wireless / QR Device Hub):
.\run.bat

# Open directly in Offline Keymap Studio (no phone required):
.\PhantomTouch.exe --demo

# Optional developer console for live stdout logs:
.\PhantomTouch.exe --console
```

---

## Hotkeys & Everyday Shortcuts

| Shortcut | Action |
| :--- | :--- |
| **`Ctrl + C / V / X / A / Z`** *(Cursor / Typing Mode)* | Native bidirectional copy, paste, cut, select-all, and undo on your phone |
| **`Ctrl + Mouse Wheel`** *(Cursor Mode)* | Smooth 2-finger pinch-zoom in Maps, Gallery, Browser, and strategy games |
| **Right-Click on Phone** *(Cursor Mode)* | Android **Back** navigation (toggleable in `DEVICE`) |
| **Drag & Drop File onto Window** | `.apk` installs via `adb install -r`; any other file pushes to `/sdcard/Download/` |
| **`~`** (configurable) or **`F1`** | Toggle **Aim Lock** (360° Raw Mouse) ↔ **Cursor Mode** (in Game Keymap profiles) |
| **`F2`** or **`KEYS` Rail Button** | Open / Close **Outside-Viewport Keymap Studio** |
| **`F3`** / **`F4`** / **`F5`** | Cycle Fire Mode / Toggle HUD Badges / Reset all virtual touches |
| **`F11`** or **`FULL` Rail Button** | Toggle **Immersive Fullscreen Mode** |
| **Double-Click Key** *(in Keymap Studio)* | Rebind key (Keyboard / Mouse / Scroll Up / Scroll Down / 2-Key Combo) |
