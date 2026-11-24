# Scrcpy Keymapper Pro (100% PC-Side C++ Wrapper)

A lightweight, native Win32 C++ mouse & keyboard multi-touch keymapper built around official `scrcpy`.
**Zero Android code is written or installed on your phone** — all input capture, independent X/Y sensitivity math, fire-drag physics, multi-touch scheduling, and visual HUD editing run 100% on Windows.

> **Important Notice for Online Games:** Using external mouse/keyboard keymappers in competitive mobile games (such as Free Fire) may violate the game's Terms of Service and risk account flags or bans. Use responsibly and avoid testing on a main account.

---

## Features

- **100% PC-Side Code (Unmodified `scrcpy`):** Uses standard `adb.exe`, `scrcpy.exe`, and `scrcpy-server`.
- **Windows Raw Mouse Input (`WM_INPUT`) + 120–240 Hz Coalescing:** Reads raw mouse counts at 1000 Hz without Windows pointer acceleration and flushes clean multi-touch packets at your phone's refresh rate (eliminates crosshair jitter).
- **Independent X / Y Sensitivity + Separate Fire X / Y Multipliers:**
  - `sens_x` / `sens_y` for camera tracking
  - `fire_sens_x` / `fire_sens_y` applied while Fire is held (e.g., tuned vertical drag speed for drag shots)
  - Sub-pixel floating-point accumulator (zero dropped micro-movements)
  - Optional dynamic velocity curve (`accel_power`) and exponential anti-jitter smoothing (`smoothing`)
- **Two Fire Modes (`F3` to toggle live):**
  - **Drag-on-Fire (`fire_drag_mode = 1`):** Left-Click presses the Right Fire button and routes mouse X/Y drag directly onto the Fire finger (just like thumb drag on a phone).
  - **Separate Fire (`fire_drag_mode = 0`):** Left-Click presses the Left Fire button on its own `pointer_id` while Aim stays uninterrupted on the Camera `pointer_id`.
- **Smart Anchor Box & Idle Auto-Recenter:** Infinite 360° camera rotation with automatic edge reset + idle auto-recenter so you always have maximum vertical drag space ready.
- **Auto-Rotation Support (Built-in H.264 SPS Parser):** Automatically detects when your phone switches between Portrait and Landscape and updates 1:1 native touch coordinates immediately.
- **Visual Drag-and-Drop HUD & X/Y Editor (`F2`):** Transparent always-on-top overlay pinned to the `scrcpy` mirror window — drag buttons onto your exact in-game HUD, resize boxes with the scroll wheel, tune X/Y live, and save with `Ctrl+S`.
- **Offline Demo Mode (`--demo`):** Test Aim Mode, HUD drag-and-drop, and X/Y settings even without plugging in a phone.

---

## Quick Start

### 1. Download Official `scrcpy` (One-Time Setup)
Run the included PowerShell helper script to download the latest official Windows `scrcpy` release into `.\scrcpy\`:
```powershell
powershell -ExecutionPolicy Bypass -File .\setup_scrcpy.ps1
```
*(Or manually unzip official `scrcpy` from [github.com/Genymobile/scrcpy/releases](https://github.com/Genymobile/scrcpy/releases) into the `scrcpy\` folder next to `scrcpy_keymapper.exe`.)*

### 2. Enable USB Debugging on Your Phone
1. On Android: **Settings → About Phone** → tap **Build Number** 7 times.
2. **Settings → Developer Options** → turn on **USB Debugging**.
   *(On Xiaomi / MIUI / HyperOS / POCO, also enable **USB Debugging (Security Settings)** so Android allows simulated touch events.)*
3. Plug your phone in via USB and tap **Allow** on the USB debugging prompt.

### 3. Run
```powershell
# Live phone mode (opens scrcpy display + transparent overlay):
.\scrcpy_keymapper.exe

# Offline Demo / HUD Editor mode (no phone required):
.\scrcpy_keymapper.exe --demo
```

---

## Hotkeys

| Hotkey | Action |
| :--- | :--- |
| **`~`** (Tilde) or **`F1`** | Toggle **Aim Mode** (locked 360° Raw Mouse) ↔ **Cursor Mode** (direct screen clicks for lobby/looting) |
| **`F2`** | Toggle **Visual HUD & X/Y Editor** (drag & drop buttons over your live game HUD) |
| **`F3`** | Toggle **Drag-on-Fire** (Right Fire) ↔ **Separate Fire** (Left Fire) |
| **`F4`** | Show / Hide key badges while playing |
| **`F5`** | Quick Reset all virtual fingers (anti-stuck failsafe) |
| **`[` / `]`** | Decrease / Increase **X Sensitivity** live (`±0.05`) |
| **`-` / `=`** | Decrease / Increase **Y Sensitivity** live (`±0.05`) |
| **`Tab` / `M`** | Taps Bag / Map and automatically frees the mouse cursor (tap again to snap back to Aim Mode) |
| **`Ctrl + S`** *(in F2 Edit Mode)* | Save your layout & X/Y settings to `profiles\freefire.cfg` |

### In `F2` Edit Mode
- **Left-Drag** any button, `WASD` circle, `AIM` anchor, or `FIRE` node to position it over your HUD.
- **Mouse Wheel** over `WASD`, `AIM`, or `FIRE` resizes its radius / safe box (`Shift + Wheel` adjusts `WASD` sprint ring or `AIM` vertical box height).
- **Right-Click** anywhere to add a new button and press any key to bind it.
- Click a button and press **`Enter`** to rebind it, or **`Delete`** to remove it.

---

## Rebuilding from Source

Requires only MinGW `g++` (zero external libraries):
```bat
build.bat
```
