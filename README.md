# ChromaSense

A wireless colorimeter I built for my M.Eng. project: a custom PCB around the TCS3530 colour sensor, an ESP32 that reads it and does the colour-science math, and a Python GUI that plots the result live on a CIE 1931 diagram. Parts cost works out to around €62 — commercial instruments that do roughly the same job run €1,500 to €6,000+.

![ChromaSense device showing live illuminance, CCT and CIE chromaticity readings next to the custom breakout board](assets/hero.jpg)

## What it does

Measures illuminance (lux), correlated colour temperature (CCT via McCamy's approximation), and CIE 1931 *xy* chromaticity, all live, streamed over Bluetooth to a desktop GUI at 10 Hz.

| | |
|---|---|
| Sensor | ams-OSRAM TCS35303-2M, 24-bit XYZ tristimulus |
| Board | 2-layer breakout I designed in KiCad, 25 × 30 mm |
| MCU | M5Stack Core2 (ESP32), I²C @ 400 kHz |
| Link | Bluetooth SPP, JSON @ 10 Hz |
| GUI | PyQt5, live CIE 1931 diagram |

## Why there's a custom PCB at all

The TCS3530 runs on 1.8 V and comes in an OLGA package — you can't just dead-bug it onto a 3.3 V dev board. So the breakout carries the sensor, its own LDO, decoupling caps, and a Grove connector that plugs straight into the Core2's I²C port.

<img src="assets/pcb-photo.jpg" width="45%" alt="Assembled ChromaSense Breakout v1.1"> <img src="assets/pcb-layout.png" width="45%" alt="KiCad PCB layout">

| Ref | Part | Why |
|-----|------|-----|
| U1 | TCS35303-2M | the sensor |
| U2 | AP2112K-1.8TRG1 | 1.8 V LDO for the sensor |
| C1–C4 | 4.7 µF / 1 µF 0402 MLCC | decoupling |
| J1 | Grove HY2.0-4P | I²C to the Core2 |

2-layer, DRC clean, fabbed at JLCPCB. Optical stack follows ams-OSRAM's design guide: 3.12 mm aperture, 0.20 ± 0.15 mm air gap above the die.

## Firmware

PlatformIO, ESP32, M5Unified. The sensor driver (`firmware/lib/ChromaSense_TCS3530`) is mine, written against the datasheet — more on why below.

Readings go through ams-OSRAM's factory calibration for this specific sensor: normalise for integration time, normalise per-channel gain to a common reference, then a 3×4 matrix that maps raw counts to CIE XYZ. From there it's arithmetic — x = X/(X+Y+Z), y = Y/(X+Y+Z), McCamy's cubic for CCT, photopic-weighted Y for lux.

The calibration numbers for this sensor (serial 557D0C06B8) are in `main.cpp` as plain constants. My supervisor's fine with that — what's not in here is ams-OSRAM's calibration file, their documentation, or their driver. Just the numbers I ended up using.

Also in there: an on-device dashboard on the Core2's screen, battery monitoring, and a dark-mode toggle (long-press a button, or send a command from the GUI) that kills the backlight for low-light readings without touching the sensor bus.

## The bug that ate a week

First pass, I used a library someone else had written for a TCS3530-adjacent board. Every reading came back near zero no matter how bright the light was, and changing the gain setting in code did nothing. No errors anywhere — it just didn't work, and nothing told me why.

Took a while to realise it was actually three bugs stacked on top of each other:

1. The library wrote the gain and integration-time settings to the wrong register addresses. The integration-time one mattered most — with it never actually set, the sensor was integrating for basically no time at all, so of course everything read near zero. This is the one that made it *look* like a hardware fault.
2. Even once that's fixed, the library only read 16 bits of what's a 24-bit value per channel. It'll still give you numbers — just truncated, wrong ones that look fine at a glance.
3. The FIFO frame size isn't fixed — it depends on exactly which channels and status bytes you've configured it to write. The library assumed a layout that didn't match my config, which misaligns every read after the first.

And one more thing that cost me a morning during bring-up: you have to enable and confirm the oscillator before most registers are even readable, but the device-ID register works regardless. So the obvious "is it alive?" check — read the ID, see if it matches — passes even when the sensor is completely unresponsive to everything else. Learned that one the hard way.

Once I understood what was actually wrong, writing a driver from the datasheet directly was faster than debugging someone else's. Correct addresses, explicit 24-bit FIFO mode, frame handling that matches my actual config, and a proper oscillator-ready wait before touching anything. After that the gain and integration-time settings behaved, and the calibration numbers finally meant something.

The real lesson: code that compiles and spits out plausible numbers can still be completely wrong. A reading that doesn't move when it should is a better signal than one that just looks off. And the cheapest sanity check I had — pinging the ID register — was the one giving me a false "yes, it's working."

## The GUI

![ChromaSense PyQt5 dashboard with live CIE 1931 chromaticity diagram](assets/gui-dashboard.png)

PyQt5 app, live chromaticity plot with telemetry panels for lux, CCT, raw channel counts, and battery, plus CSV logging.

The diagram itself isn't a picture — the spectral locus is a cubic spline through the actual CIE colour-matching function data (plain linear interpolation visibly facets the curve near the green corner), and the gamut fill is computed per-pixel through an XYZ→sRGB conversion with proper gamma correction. No downloaded chart image, which also sidesteps any question of whose picture it was.

A couple of other things I had to fix along the way: Qt re-parses a stylesheet every time you call `setStyleSheet`, and at 10 Hz with colour-graded labels that adds up — caching the last colour and skipping no-op updates fixed the stutter. And the Bluetooth read loop runs on its own `QThread` so it doesn't block the UI.

## Cost

| | € |
|---|---|
| TCS35303-2M | 8.50 |
| AP2112K-1.8TRG1 | 0.45 |
| Decoupling caps ×4 | 0.20 |
| Grove connector | 0.35 |
| PCB (JLCPCB, 5 off, shipped) | ≈3.00 |
| **Breakout total** | **≈12.50** |
| M5Stack Core2 | 50.00 |
| **Per unit** | **≈62.50** |

For comparison, a [Sekonic C-800](https://galaxus.at/en/s1/product/sekonic-c-800-spectromaster-flash-unit-battery-flash-accessories-11488106) is €1,470, and a [Konica Minolta CL-200A](https://store.imatest.com/minolta-cl-200a-chroma-meter.html) is $6,500. To be clear, I'm not claiming this matches either — the C-800 is a real spectrometer with traceable calibration across the visible range, and this is one calibrated tristimulus sensor. What it does show is that you can get usable CIE coordinates for a small fraction of the cost if you're willing to build it yourself.

## What it doesn't do yet

- The lux output hasn't been checked against a reference meter, so treat absolute values as provisional — relative readings are fine.
- The sensor's Z channel is UV-sensitive enough (Z/Y swings from ~0.49 in direct sun to ~8.2 under open sky) that narrow-band sources like monitor primaries fall outside what the calibration was fitted for.
- There's no enclosure. The 3D-printed housing got cut for time, so right now it's a bare board and stray light is whatever it is.
- The calibration is for one specific sensor (557D0C06B8). A different unit needs its own factory numbers from ams-OSRAM.

## Layout

```
chromasense/
├── hardware/     KiCad project, Gerbers, footprints, STEP model
├── firmware/     PlatformIO project
│   ├── src/main.cpp
│   └── lib/ChromaSense_TCS3530/   the driver
├── python-gui/   main_window.py, cie_widget.py, bt_worker.py
├── assets/
└── requirements.txt
```

## Running it

Firmware:
```bash
cd firmware
pio run --target upload
```
Calibration for sensor 557D0C06B8 is already in `src/main.cpp`. Swap in your own sensor's numbers from ams-OSRAM if you're using a different unit.

GUI:
```bash
cd python-gui
pip install -r ../requirements.txt
python main_window.py
```
Pair the Core2 over Bluetooth, pick the right COM port in the dropdown, connect.

---

M.Eng. Electrical and Microsystems Engineering, OTH Regensburg, Faculty ANK. Supervised by Prof. Johannes Wild.

**Vivek Wilson Neelamkavil** · [LinkedIn](#) · [CV](#)

TCS3530 references throughout are to the public datasheet (ams-OSRAM DS001080 v3-00). No ams-OSRAM documentation, software, or raw calibration files are included here.
