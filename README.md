# Dynamic Cursor

This app makes your cursor more realistic by simulating how it would behave if it was an actual object being dragged across your screen. This means that your cursor can change based on how it is used, e.g. stretch in the direction you are moving or straight out rotate towards it.

This project is inspired by [VirtCode/hypr-dynamic-cursors](https://github.com/VirtCode/hypr-dynamic-cursors) but in windows using c++

---

## Simulation modes

The overlay includes two unique physics simulation modes toggled via the system tray. (if you were wandering the cursor shown are the Bibata Cursor)

| Stretch Animation Mode | Rotate Animation Mode |
| :---: | :---: |
| `<video src="assets/Stretch.mp4" width="350" controls mute loop></video>` | `<video src="assets/Rotate.mp4" width="350" controls mute loop></video>` |

---

## The Stack used

- **Language:** C++17 / C++20
- **Graphics Pipeline:** Direct2D, Windows Imaging Component (wincodec)
- **Windowing Context:** Win32 API Layered Windows (WS_EX_LAYERED, WS_EX_TRANSPARENT), Desktop Window Manager (dwmapi)
- **Build Configurations:** Linker dependencies include: `d2d1.lib`, `Windowscodecs.lib`, `ole32.lib`, `winmm.lib`, `dwmapi.lib`, `shell32.lib`, `comdlg32.lib`

---

## ⚙️ Configuration (`settings.ini`)

On initialization, the app creates a config folder containing a default `settings.ini` profile to modify physics attributes:

```ini
[Physics]
SimulationMode=0    ; 0 = Stretch (cursor squashes when moving fast), 1 = Rotate (cursor spins toward movement)
StiffnessFar=0.65   ; How fast cursor catches up when moving far away (0=slow/laggy, 1=instant)
StiffnessMedium=0.42; How fast cursor catches up during normal movement (0=slow, 1=instant)
StiffnessClose=0.24 ; How smooth cursor feels for small, precise movements (lower = smoother)
StretchFactor=0.025 ; How much the cursor stretches when moving (higher = more stretch)
MaxStretch=2.2      ; Maximum how far the cursor can stretch (prevents extreme distortion)